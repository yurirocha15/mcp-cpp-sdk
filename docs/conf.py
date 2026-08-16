# Configuration file for the Sphinx documentation builder.

import os
import re
import sys
from pathlib import Path

from docutils.parsers.rst import Directive, directives

# -- Project information -----------------------------------------------------
project = 'mcp-cpp-sdk'
copyright = '2026, MCP C++ SDK Contributors'
author = 'MCP C++ SDK Contributors'

_version_text = (Path(__file__).resolve().parents[1] / 'VERSION').read_text().strip()
_version_match = re.fullmatch(
    r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
    r'(?:-rc\.([1-9][0-9]*))?',
    _version_text,
)
if _version_match is None:
    raise RuntimeError(f'Invalid canonical VERSION: {_version_text!r}')

# The short X.Y version and complete stable/RC SemVer come from the canonical
# repository VERSION file.
version = f'{_version_match.group(1)}.{_version_match.group(2)}'
release = _version_text

# -- General configuration ---------------------------------------------------

# Conditionally enable Breathe only when Doxygen XML is available.
_doc_dir = os.path.dirname(__file__)
_possible_xml_dirs = [
    os.path.join(_doc_dir, '..', 'build', 'doxygen', 'xml'),
    os.path.join(_doc_dir, '..', 'build', 'release', 'doxygen', 'xml'),
    os.path.join(_doc_dir, '..', 'build', 'debug', 'doxygen', 'xml'),
    os.path.join(_doc_dir, '..', 'build', 'sanitize', 'doxygen', 'xml'),
    os.path.join(_doc_dir, '..', 'build', 'coverage', 'doxygen', 'xml'),
]

_doxygen_xml_dir = None
for _dir in _possible_xml_dirs:
    if os.path.isfile(os.path.join(_dir, 'index.xml')):
        _doxygen_xml_dir = _dir
        break

_has_doxygen = _doxygen_xml_dir is not None

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

# The name of the Pygments (syntax highlighting) style to use.
pygments_style = 'sphinx'

# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.
html_theme = 'sphinx_rtd_theme'

# Theme options are theme-specific and customize the look and feel of a theme
# further.
html_theme_options = {
    'logo_only': False,
    'prev_next_buttons_location': 'bottom',
    'style_external_links': False,
    # Toc options
    'collapse_navigation': False,
    'sticky_navigation': True,
    'navigation_depth': 4,
    'includehidden': True,
    'titles_only': False
}

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['_static']

# Add custom CSS files
html_css_files = [
    'css/custom.css',
]

# -- Options for HTMLHelp output ---------------------------------------------

# Output file base name for HTML help builder.
htmlhelp_basename = 'mcp-cpp-sdkdoc'


class _UnavailableDoxygenDirective(Directive):
    """Keep the API overview buildable when Doxygen XML is unavailable."""

    required_arguments = 1
    option_spec = {
        'members': directives.flag,
        'undoc-members': directives.flag,
    }

    def run(self):
        return []


def setup(app):
    """Install inert Doxygen directives for the documented fallback build."""
    if not _has_doxygen:
        for name in ('doxygenclass', 'doxygenstruct', 'doxygentypedef'):
            app.add_directive(name, _UnavailableDoxygenDirective)
