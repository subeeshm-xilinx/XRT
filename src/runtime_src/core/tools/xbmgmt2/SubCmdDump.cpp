// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2021-2022 Xilinx, Inc
// Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.

// ------ I N C L U D E   F I L E S -------------------------------------------
// Local - Include Files
#include "SubCmdDump.h"
#include "tools/common/XBUtilitiesCore.h"
#include "tools/common/XBUtilities.h"
namespace XBU = XBUtilities;

#include "core/common/query_requests.h"
#include "flash/flasher.h"

// 3rd Party Library - Include Files
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

// System - Include Files
#include <iostream>
#include <fstream>

// ------ L O C A L   F U N C T I O N S ---------------------------------------

static void
flash_dump(const std::shared_ptr<xrt_core::device>& _dev, const std::string m_output)
{
  // Sample m_output:
  //   m_output file: foo.bin
  //   Flash Size: 0x222 (Mbits)
  //   <Progress Bar>

  Flasher flasher(_dev->get_device_id());
  if(!flasher.isValid()) {
    xrt_core::error(boost::str(boost::format("%d is an invalid index") % _dev->get_device_id()));
    return;
  }
  try{
    flasher.readBack(m_output);
  } catch(const std::exception& ex){
      std::cerr << "  ERROR: " << ex.what() << std::endl << std::endl;
      throw xrt_core::error(std::errc::operation_canceled);
  }
}

static bool
is_supported(const std::shared_ptr<xrt_core::device>& dev)
{
  bool is_mfg = false;
  bool is_recovery = false;

  try {
    is_mfg = xrt_core::device_query<xrt_core::query::is_mfg>(dev);
  } catch(const xrt_core::query::exception&) {}

  try {
    is_recovery = xrt_core::device_query<xrt_core::query::is_recovery>(dev);
  } catch(const xrt_core::query::exception&) {}

  if (is_mfg || is_recovery) {
    std::cerr << boost::format("This operation is not supported with %s image.\n") % (is_mfg ? "manufacturing" : "recovery");
    return false;
  }

  return true;
}

/*
 * so far, we only support the following configs, eg
 * [Device]
 * mailbox_channel_disable = 0x20
 * mailbox_channel_switch = 0
 * xclbin_change = 1
 * cache_xclbin = 0
 */
static void
config_dump(const std::shared_ptr<xrt_core::device>& _dev, const std::string m_output)
{
  boost::property_tree::ptree ptRoot;

  boost::property_tree::ptree child;
  child.put("mailbox_channel_disable", xrt_core::device_query<xrt_core::query::config_mailbox_channel_disable>(_dev));
  child.put("mailbox_channel_switch", xrt_core::device_query<xrt_core::query::config_mailbox_channel_switch>(_dev));
  child.put("xclbin_change", xrt_core::device_query<xrt_core::query::config_xclbin_change>(_dev));
  child.put("cache_xclbin", xrt_core::device_query<xrt_core::query::cache_xclbin>(_dev));

  if (is_supported(_dev)) {
    bool is_versal = xrt_core::device_query<xrt_core::query::is_versal>(_dev);
    if (is_versal) {
      try {
        child.put("scaling_enabled", xrt_core::device_query<xrt_core::query::xgq_scaling_enabled>(_dev));
        child.put("scaling_power_override", xrt_core::device_query<xrt_core::query::xgq_scaling_power_override>(_dev));
        child.put("scaling_temp_override", xrt_core::device_query<xrt_core::query::xgq_scaling_temp_override>(_dev));
      } catch(const xrt_core::query::exception&) {}
    } else {
      try {
        child.put("scaling_enabled", xrt_core::device_query<xrt_core::query::xmc_scaling_enabled>(_dev));
        child.put("scaling_power_override", xrt_core::device_query<xrt_core::query::xmc_scaling_power_override>(_dev));
        child.put("scaling_temp_override", xrt_core::device_query<xrt_core::query::xmc_scaling_temp_override>(_dev));
      } catch(const xrt_core::query::exception&) {}
    }
  }

  ptRoot.put_child("Device", child);

  boost::property_tree::ini_parser::write_ini(m_output, ptRoot);
  std::cout << "config has been dumped to " << m_output << std::endl;
}

SubCmdDump::SubCmdDump(bool _isHidden, bool _isDepricated, bool _isPreliminary)
    : SubCmd("dump",
             "Dump out the contents of the specified option")
    , m_device("")
    , m_output("")
    , m_flash(false)
    , m_config(false)
    , m_help(false)
{
  const std::string longDescription = "Dump out the contents of the specified option.";
  setLongDescription(longDescription);
  setExampleSyntax("");
  setIsHidden(_isHidden);
  setIsDeprecated(_isDepricated);
  setIsPreliminary(_isPreliminary);

  m_commonOptions.add_options()
    ("device,d", boost::program_options::value<decltype(m_device)>(&m_device), "The Bus:Device.Function (e.g., 0000:d8:00.0) device of interest.")
    ("config,c", boost::program_options::bool_switch(&m_config), "Dumps the m_output of system configuration, requires a .ini m_output file by -o option")
    ("flash,f", boost::program_options::bool_switch(&m_flash), "Dumps the m_output of programmed system image, requires a .bin m_output file by -o option")
    ("output,o", boost::program_options::value<decltype(m_output)>(&m_output), "Direct the m_output to the given file")
    ("help", boost::program_options::bool_switch(&m_help), "Help to use this sub-command")
  ;
}

// ----- C L A S S   M E T H O D S -------------------------------------------

void
SubCmdDump::execute(const SubCmdOptions& _options) const
{
  XBU::verbose("SubCommand: dump");

  // Parse sub-command ...
  po::variables_map vm;
  process_arguments(vm, _options);

  // Check to see if help was requested or no command was found
  if (m_help)  {
    printHelp();
    return;
  }

  // -- Now process the subcommand --------------------------------------------
  XBU::verbose("SubCmd: Dump");

  // -- process "device" option -----------------------------------------------
  XBU::verbose("Option: device");
  for (auto & str : m_device)
    XBU::verbose(std::string(" ") + str);

  // Find device of interest
  std::shared_ptr<xrt_core::device> device;

  try {
    device = XBU::get_device(m_device, false /*inUserDomain*/);
  } catch (const std::runtime_error& e) {
    // Catch only the exceptions that we have generated earlier
    std::cerr << boost::format("ERROR: %s\n") % e.what();
    throw xrt_core::error(std::errc::operation_canceled);
  }

  // -- process "output" option -----------------------------------------------
  // output file
  XBU::verbose("Option: output: " + m_output);

  if (m_output.empty()) {
    std::cerr << "ERROR: Please specify an output file using --output option" << "\n\n";
    printHelp();
    throw xrt_core::error(std::errc::operation_canceled);
  }
  if (!m_output.empty() && boost::filesystem::exists(m_output) && !XBU::getForce()) {
    std::cerr << boost::format("m_output file already exists: '%s'") % m_output << "\n\n";
    throw xrt_core::error(std::errc::operation_canceled);
  }

  //decide the contents of the dump file
  if(m_flash) {
    flash_dump(device, m_output);
    return;
  }
  if (m_config) {
    config_dump(device, m_output);
    return;
  }

  std::cerr << "ERROR: Please specify a valid option to determine the type of dump" << "\n\n";
  printHelp();
  throw xrt_core::error(std::errc::operation_canceled);
}
