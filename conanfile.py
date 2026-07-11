from pathlib import Path

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class McpCppSdkConan(ConanFile):
    name = "mcp-cpp-sdk"
    package_type = "library"
    license = "Apache-2.0"
    url = "https://github.com/yurirocha15/mcp-cpp-sdk"
    homepage = "https://github.com/yurirocha15/mcp-cpp-sdk"
    description = "C++20 Model Context Protocol SDK"
    topics = ("mcp", "model-context-protocol", "cpp20")
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {
        "shared": False,
        "fPIC": True,
        "boost/*:shared": False,
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
    exports_sources = (
        "CMakeLists.txt",
        "VERSION",
        "LICENSE",
        "cmake/*",
        "include/*",
        "src/*",
    )

    def set_version(self):
        version_file = Path(self.recipe_folder) / "VERSION"
        self.version = version_file.read_text(encoding="utf-8").strip()

    def requirements(self):
        self.requires(
            "boost/1.86.0", transitive_headers=True, transitive_libs=True
        )
        self.requires("nlohmann_json/3.12.0", transitive_headers=True)
        self.requires(
            "openssl/3.6.1", transitive_headers=True, transitive_libs=True
        )

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, "20")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["BUILD_TESTING"] = False
        tc.variables["BUILD_EXAMPLES"] = False
        tc.variables["BUILD_DOCS"] = False
        tc.variables["MCP_CPP_SDK_BUILD_SHARED"] = bool(self.options.shared)
        tc.variables["MCP_CPP_SDK_BUILD_STATIC"] = not bool(self.options.shared)
        tc.variables["MCP_CPP_SDK_STATIC_PIC"] = bool(
            self.options.get_safe("fPIC", False)
        )
        tc.variables["MCP_CPP_SDK_DEFAULT_LINKAGE"] = (
            "shared" if self.options.shared else "static"
        )
        if self.options.get_safe("fPIC") is not None:
            tc.variables["CMAKE_POSITION_INDEPENDENT_CODE"] = bool(self.options.fPIC)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=Path(self.package_folder) / "licenses",
        )

    def package_info(self):
        self.cpp_info.libs = [
            "mcp-cpp-sdk" if self.options.shared else "mcp-cpp-sdk-static"
        ]
        self.cpp_info.requires = [
            "boost::headers",
            "nlohmann_json::nlohmann_json",
            "openssl::crypto",
        ]
        self.cpp_info.defines.append("MCP_DLL" if self.options.shared else "MCP_STATIC")
        self.cpp_info.set_property("cmake_file_name", "mcp-cpp-sdk")
        self.cpp_info.set_property("cmake_target_name", "mcp::sdk")
