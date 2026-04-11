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
