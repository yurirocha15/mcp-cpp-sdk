from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class McpCppSdkConan(ConanFile):
    name = "mcp-cpp-sdk"
    version = "0.1.0"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    requires = (
        "boost/1.86.0",
        "nlohmann_json/3.12.0",
        "openssl/3.6.1",
    )

    default_options = {
        "boost/*:without_python": True,
        "boost/*:without_mpi": True,
        "boost/*:without_graph": True,
        "boost/*:without_graph_parallel": True,
        "boost/*:without_iostreams": True,
        "boost/*:without_locale": True,
        "boost/*:without_log": True,
        "boost/*:without_program_options": True,
        "boost/*:without_random": True,
        "boost/*:without_regex": True,
        "boost/*:without_serialization": True,
        "boost/*:without_test": True,
        "boost/*:without_type_erasure": True,
        "boost/*:without_wave": True,
        "boost/*:without_math": True,
        "boost/*:without_contract": True,
        "boost/*:without_nowide": True,
        "boost/*:without_stacktrace": True,
        "boost/*:without_cobalt": True,
        "boost/*:without_context": True,
        "boost/*:without_coroutine": True,
        "boost/*:without_json": True,
        "boost/*:without_fiber": True,
    }

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()

    def package_info(self):
        self.cpp_info.libs = ["mcp-cpp-sdk"]
