# Configuration file for the Sphinx documentation builder.

import os
import sys

# -- Project information -----------------------------------------------------
project = 'mcp-cpp-sdk'
copyright = '2026, MCP C++ SDK Contributors'
author = 'MCP C++ SDK Contributors'

# The short X.Y version
version = '0.1'
# The full version, including alpha/beta/rc tags
release = '0.1.0'

# -- General configuration ---------------------------------------------------

# Conditionally enable Breathe only when Doxygen XML is available.
_doxygen_xml_dir = os.path.join(os.path.dirname(__file__), '..', 'build', 'doxygen', 'xml')
_has_doxygen = os.path.isfile(os.path.join(_doxygen_xml_dir, 'index.xml'))

extensions = [
    'sphinx.ext.autodoc',
    'sphinx.ext.viewcode',
]

if _has_doxygen:
    extensions.append('breathe')
    breathe_projects = {
        'mcp-cpp-sdk': _doxygen_xml_dir,
    }
    breathe_default_project = 'mcp-cpp-sdk'

# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# Exclude API reference pages when Doxygen XML is not available.
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']
if not _has_doxygen:
    exclude_patterns.append('api')

# The name of the Pygments (syntax highlighting) style to use.
pygments_style = 'sphinx'

# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.
html_theme = 'furo'

# Theme options are theme-specific and customize the look and feel of a theme
# further.
html_theme_options = {
    "sidebar_hide_name": False,
}

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = []

# -- Options for HTMLHelp output ---------------------------------------------

# Output file base name for HTML help builder.
htmlhelp_basename = 'mcp-cpp-sdkdoc'
