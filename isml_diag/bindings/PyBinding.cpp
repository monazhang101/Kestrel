#include "diag/device/DeviceManager.h"

#include <cstdint>
#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

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
        d["children"] = device.children;
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
    m.doc() = "Python bindings for TPU diagnostic HAL pseudocode";

    py::class_<DeviceContext>(m, "DeviceContext")
        .def(py::init<>())
        .def_readwrite("bdf", &DeviceContext::bdf)
        .def_readwrite("vendor_id", &DeviceContext::vendor_id)
        .def_readwrite("device_id", &DeviceContext::device_id)
        .def_readwrite("bar_device_base", &DeviceContext::bar_device_base)
        .def_readwrite("bar_size", &DeviceContext::bar_size)
        .def_property(
            "mapped_bar_base",
            [](const DeviceContext& ctx) {
                return reinterpret_cast<uintptr_t>(ctx.mapped_bar_base);
            },
            [](DeviceContext& ctx, uintptr_t addr) {
                ctx.mapped_bar_base = reinterpret_cast<void*>(addr);
            });

    py::class_<TestResult>(m, "TestResult")
        .def(py::init<>())
        .def_readwrite("test_name", &TestResult::test_name)
        .def_readwrite("target_name", &TestResult::target_name)
        .def_readwrite("passed", &TestResult::passed)
        .def_readwrite("metrics", &TestResult::metrics)
        .def_readwrite("error_description", &TestResult::error_description)
        .def_readwrite("error_details", &TestResult::error_details)
        .def("__repr__", [](const TestResult& result) {
            return "<TestResult test_name='" + result.test_name +
                   "' target_name='" + result.target_name +
                   "' passed=" + (result.passed ? "true" : "false") + ">";
        });

    py::enum_<LogLevel>(m, "LogLevel")
        .value("ERROR", LogLevel::Error)
        .value("INFO", LogLevel::Info)
        .value("DEBUG", LogLevel::Debug)
        .value("TRACE", LogLevel::Trace);

    py::enum_<HalType>(m, "HalType")
        .value("pHal", HalType::pHal)
        .value("iHal", HalType::iHal);

    py::class_<DeviceDiscoveryInfo>(m, "DeviceDiscoveryInfo")
        .def(py::init<>())
        .def_readwrite("name", &DeviceDiscoveryInfo::name)
        .def_readwrite("type", &DeviceDiscoveryInfo::type)
        .def_readwrite("parent", &DeviceDiscoveryInfo::parent)
        .def_readwrite("bdf", &DeviceDiscoveryInfo::bdf)
        .def_readwrite("vendor_id", &DeviceDiscoveryInfo::vendor_id)
        .def_readwrite("device_id", &DeviceDiscoveryInfo::device_id)
        .def_readwrite("locator", &DeviceDiscoveryInfo::locator)
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

    py::class_<BaseDevice>(m, "BaseDevice")
        .def("run_atomic_test",
             [](BaseDevice& device,
                const std::string& test_name,
                const TestArgs& args) {
                 return device.run_atomic_test(test_name, args);
             },
             py::arg("test_name"),
             py::arg("args") = TestArgs{},
             py::call_guard<py::gil_scoped_release>())
        .def("get_name", &BaseDevice::get_name)
        .def("get_registered_test_names", &BaseDevice::get_registered_test_names)
        .def("get_context",
             static_cast<DeviceContext& (BaseDevice::*)()>(&BaseDevice::get_context),
             py::return_value_policy::reference_internal)
        .def("get_bar_base_addr", [](const BaseDevice& device) {
            return reinterpret_cast<uintptr_t>(device.get_bar_base_addr());
        });

    py::class_<TPUDevice, BaseDevice>(m, "TPUDevice")
        .def("tpu_index", &TPUDevice::tpu_index)
        .def("print_tree", &TPUDevice::print_tree);

    py::class_<PCIeModule, BaseDevice>(m, "PCIeModule");
    py::class_<PMUModule, BaseDevice>(m, "PMUModule");
    py::class_<ISIModule, BaseDevice>(m, "ISIModule")
        .def("link_id", &ISIModule::link_id);
    py::class_<DMCModule, BaseDevice>(m, "DMCModule")
        .def("ddp_id", &DMCModule::ddp_id)
        .def("controller_id", &DMCModule::controller_id);
    py::class_<DDPModule, BaseDevice>(m, "DDPModule");

    py::class_<DeviceManager>(m, "DeviceManager")
        .def(py::init<>())
        .def(py::init<HalType>(), py::arg("hal_type"))
        .def("get_hal_type", &DeviceManager::get_hal_type)
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
        .def("run_atomic_test",
             [](DeviceManager& manager,
                const std::string& target_name,
                const std::string& test_name,
                const TestArgs& args) {
                 return manager.run_atomic_test(target_name, test_name, args);
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

    m.def("discover", [](HalType hal_type) {
        DeviceManager manager(hal_type);
        return device_tree_to_dict(manager.discover());
    }, py::arg("hal_type"));
}
