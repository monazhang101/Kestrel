#include "diag/device/DeviceManager.h"

#include <cstdint>
#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

static py::dict bar_mapping_to_dict(const BarMapping& bar)
{
    py::dict d;
    d["bar_index"] = bar.bar_index;
    d["name"] = bar.name;
    d["mapped_base"] = reinterpret_cast<uintptr_t>(bar.mapped_base);
    d["device_base"] = bar.device_base;
    d["expected_size"] = bar.expected_size;
    d["resource_size"] = bar.resource_size;
    d["mapped_size"] = bar.mapped_size;
    d["mapped"] = bar.mapped;
    d["writable"] = bar.writable;
    d["error"] = bar.error;
    d["layout"] = bar.layout;
    return d;
}

static py::dict device_tree_to_dict(const DeviceTree& tree)
{
    py::list devices;
    for (const auto& device : tree.devices) {
        py::dict d;
        d["name"] = device.name;
        d["type"] = device.type;
        d["parent"] = device.parent;
        d["bdf"] = device.bdf;
        d["vendor_id"] = device.vendor_id;
        d["device_id"] = device.device_id;
        d["locator"] = device.locator;
        d["properties"] = device.properties;
        d["children"] = device.children;
        py::list bars;
        for (const auto& bar : device.bars) {
            bars.append(bar_mapping_to_dict(bar));
        }
        d["bars"] = bars;
        devices.append(d);
    }

    py::list topology;
    for (const auto& edge : tree.topology) {
        py::dict e;
        e["parent"] = edge.parent;
        e["child"] = edge.child;
        e["type"] = edge.type;
        topology.append(e);
    }

    py::dict result;
    result["devices"] = devices;
    result["topology"] = topology;
    return result;
}

PYBIND11_MODULE(tpu_hal, m)
{
    m.doc() = "Python bindings for the TPU diagnostic framework";

    py::class_<BarMapping>(m, "BarMapping")
        .def(py::init<>())
        .def_readwrite("bar_index", &BarMapping::bar_index)
        .def_readwrite("name", &BarMapping::name)
        .def_readwrite("device_base", &BarMapping::device_base)
        .def_readwrite("expected_size", &BarMapping::expected_size)
        .def_readwrite("resource_size", &BarMapping::resource_size)
        .def_readwrite("mapped_size", &BarMapping::mapped_size)
        .def_readwrite("mapped", &BarMapping::mapped)
        .def_readwrite("writable", &BarMapping::writable)
        .def_readwrite("error", &BarMapping::error)
        .def_readwrite("layout", &BarMapping::layout)
        .def_property(
            "mapped_base",
            [](const BarMapping& bar) {
                return reinterpret_cast<uintptr_t>(bar.mapped_base);
            },
            [](BarMapping& bar, uintptr_t addr) {
                bar.mapped_base = reinterpret_cast<void*>(addr);
            });

    py::class_<DeviceContext>(m, "DeviceContext")
        .def(py::init<>())
        .def_readwrite("bdf", &DeviceContext::bdf)
        .def_readwrite("vendor_id", &DeviceContext::vendor_id)
        .def_readwrite("device_id", &DeviceContext::device_id)
        .def_readwrite("bar_mappings", &DeviceContext::bar_mappings);

    py::enum_<TestStatus>(m, "TestStatus")
        .value("OK", PHAL_STATUS_OK)
        .value("TIMEOUT", PHAL_STATUS_TIMEOUT)
        .value("ERROR", PHAL_STATUS_ERROR)
        .value("UNIMPLEMENTED", PHAL_STATUS_UNIMPLEMENTED)
        .value("INVALID", PHAL_STATUS_INVALID);

    py::enum_<LogLevel>(m, "LogLevel")
        .value("ERROR", LogLevel::Error)
        .value("INFO", LogLevel::Info)
        .value("FLOW", LogLevel::Flow)
        .value("REGIO", LogLevel::RegIo)
        .value("DEBUG", LogLevel::Flow)
        .value("TRACE", LogLevel::RegIo);

    py::enum_<AtlasDmcType>(m, "AtlasDmcType")
        .value("FAKE_DMEM", AtlasDmcType::FakeDmem)
        .value("CXL20", AtlasDmcType::Cxl20)
        .value("CXL30", AtlasDmcType::Cxl30);

    py::enum_<AtlasPlatform>(m, "AtlasPlatform")
        .value("CHIP", AtlasPlatform::Chip)
        .value("EMU", AtlasPlatform::Emu)
        .value("FPGA", AtlasPlatform::Fpga)
        .value("UNKNOWN", AtlasPlatform::Unknown);

    py::enum_<AtlasJtagMux>(m, "AtlasJtagMux")
        .value("PMU", AtlasJtagMux::Pmu)
        .value("CHAIN1", AtlasJtagMux::Chain1)
        .value("CHAIN4", AtlasJtagMux::Chain4)
        .value("UNKNOWN", AtlasJtagMux::Unknown);

    py::enum_<AtlasAicProfile>(m, "AtlasAicProfile")
        .value("ACC_ONLY", AtlasAicProfile::AccOnly)
        .value("EMU_CUT1", AtlasAicProfile::EmuCut1)
        .value("CUT2", AtlasAicProfile::Cut2)
        .value("FPGA_CUT3", AtlasAicProfile::FpgaCut3)
        .value("FPGA_CUT1", AtlasAicProfile::FpgaCut1)
        .value("EMU_FULL", AtlasAicProfile::EmuFull)
        .value("UNKNOWN", AtlasAicProfile::Unknown);

    py::class_<AtlasAicInfo>(m, "AtlasAicInfo")
        .def_readonly("cfg_code", &AtlasAicInfo::cfg_code)
        .def_readonly("profile", &AtlasAicInfo::profile)
        .def_readonly("block_mask", &AtlasAicInfo::block_mask)
        .def_readonly("vpe_count", &AtlasAicInfo::vpe_count)
        .def_readonly("vpe_lanes", &AtlasAicInfo::vpe_lanes)
        .def_readonly("vpe_memory_mib", &AtlasAicInfo::vpe_memory_mib)
        .def_readonly("tpe_count", &AtlasAicInfo::tpe_count)
        .def_readonly("lpe_count", &AtlasAicInfo::lpe_count);

    py::class_<AtlasResourceInfo>(m, "AtlasResourceInfo")
        .def_readonly("raw", &AtlasResourceInfo::raw)
        .def_readonly("valid", &AtlasResourceInfo::valid)
        .def_readonly("profile_matched",
                      &AtlasResourceInfo::profile_matched)
        .def_readonly("profile_name", &AtlasResourceInfo::profile_name)
        .def_readonly("platform", &AtlasResourceInfo::platform)
        .def_readonly("subsys_cfg", &AtlasResourceInfo::subsys_cfg)
        .def_readonly("cxl_cfg", &AtlasResourceInfo::cxl_cfg)
        .def_readonly("isi_cfg", &AtlasResourceInfo::isi_cfg)
        .def_readonly("aic_cfg", &AtlasResourceInfo::aic_cfg)
        .def_readonly("jtag_cfg", &AtlasResourceInfo::jtag_cfg)
        .def_readonly("dmem_size_cfg", &AtlasResourceInfo::dmem_size_cfg)
        .def_readonly("has_cxl_subsystem",
                      &AtlasResourceInfo::has_cxl_subsystem)
        .def_readonly("has_aic_extension",
                      &AtlasResourceInfo::has_aic_extension)
        .def_readonly("isi_mask", &AtlasResourceInfo::isi_mask)
        .def_readonly("aic_mask", &AtlasResourceInfo::aic_mask)
        .def_readonly("dmc_mask", &AtlasResourceInfo::dmc_mask)
        .def_readonly("cxl_mask", &AtlasResourceInfo::cxl_mask)
        .def_readonly("fake_dmem_size_bytes",
                      &AtlasResourceInfo::fake_dmem_size_bytes)
        .def_readonly("jtag_mux", &AtlasResourceInfo::jtag_mux)
        .def_readonly("aic", &AtlasResourceInfo::aic)
        .def("has_isi", &AtlasResourceInfo::has_isi)
        .def("isi_indices", &AtlasResourceInfo::isi_indices)
        .def("has_aic_blocks", &AtlasResourceInfo::has_aic_blocks)
        .def("has_dmc", &AtlasResourceInfo::has_dmc)
        .def("has_cxl", &AtlasResourceInfo::has_cxl)
        .def("dmc_type", &AtlasResourceInfo::get_dmc_type);

    py::class_<DeviceDiscoveryInfo>(m, "DeviceDiscoveryInfo")
        .def(py::init<>())
        .def_readwrite("name", &DeviceDiscoveryInfo::name)
        .def_readwrite("type", &DeviceDiscoveryInfo::type)
        .def_readwrite("parent", &DeviceDiscoveryInfo::parent)
        .def_readwrite("bdf", &DeviceDiscoveryInfo::bdf)
        .def_readwrite("vendor_id", &DeviceDiscoveryInfo::vendor_id)
        .def_readwrite("device_id", &DeviceDiscoveryInfo::device_id)
        .def_readwrite("locator", &DeviceDiscoveryInfo::locator)
        .def_readwrite("properties", &DeviceDiscoveryInfo::properties)
        .def_readwrite("bars", &DeviceDiscoveryInfo::bars)
        .def_readwrite("children", &DeviceDiscoveryInfo::children);

    py::class_<TopologyEdge>(m, "TopologyEdge")
        .def(py::init<>())
        .def_readwrite("parent", &TopologyEdge::parent)
        .def_readwrite("child", &TopologyEdge::child)
        .def_readwrite("type", &TopologyEdge::type);

    py::class_<DeviceTree>(m, "DeviceTree")
        .def(py::init<>())
        .def_readwrite("devices", &DeviceTree::devices)
        .def_readwrite("topology", &DeviceTree::topology);

    py::class_<TestTarget>(m, "TestTarget")
        .def("get_name", &TestTarget::get_name)
        .def("get_target_type", &TestTarget::get_target_type)
        .def("get_registered_test_names", &TestTarget::get_registered_test_names)
        .def("get_context",
             static_cast<DeviceContext& (TestTarget::*)()>(&TestTarget::get_context),
             py::return_value_policy::reference_internal);

    py::class_<TPUDevice, TestTarget>(m, "TPUDevice")
        .def("tpu_index", &TPUDevice::tpu_index)
        .def("resource_info", &TPUDevice::resource_info,
             py::return_value_policy::reference_internal)
        .def("has_isi", &TPUDevice::has_isi)
        .def("isi_indices", &TPUDevice::isi_indices)
        .def("dmc_mask", &TPUDevice::dmc_mask)
        .def("has_dmc", &TPUDevice::has_dmc)
        .def("cxl_mask", &TPUDevice::cxl_mask)
        .def("has_cxl", &TPUDevice::has_cxl)
        .def("dmc_type", &TPUDevice::dmc_type)
        .def("print_tree", &TPUDevice::print_tree);

    py::class_<PCIeModule, TestTarget>(m, "PCIeModule");
    py::class_<PMUModule, TestTarget>(m, "PMUModule");
    py::class_<ISIModule, TestTarget>(m, "ISIModule");
    py::class_<DDPModule, TestTarget>(m, "DDPModule");

    py::class_<DeviceManager>(m, "DeviceManager")
        .def(py::init<>())
        .def("discover", [](DeviceManager& manager) {
            return device_tree_to_dict(manager.discover());
        })
        .def("discover_tree",
             static_cast<DeviceTree (DeviceManager::*)()>(&DeviceManager::discover))
        .def("get_target",
             &DeviceManager::get_target,
             py::arg("target_name"),
             py::return_value_policy::reference_internal)
        .def("get_target_names", &DeviceManager::get_target_names)
        .def("set_log_level", &DeviceManager::set_log_level)
        .def("get_log_level", &DeviceManager::get_log_level)
        .def("run_testcase",
             [](DeviceManager& manager,
                const std::string& target_name,
                const std::string& test_name,
                const TestArgs& args) {
                 return manager.run_testcase(target_name, test_name, args);
             },
             py::arg("target_name"),
             py::arg("test_name"),
             py::arg("args") = TestArgs{},
             py::call_guard<py::gil_scoped_release>())
        .def("print_tree", &DeviceManager::print_tree);

    m.def("discover", []() {
        DeviceManager manager;
        return device_tree_to_dict(manager.discover());
    });

}
