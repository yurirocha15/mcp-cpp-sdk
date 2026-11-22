Skills
======

The Skills extension (``io.modelcontextprotocol/skills``) serves Agent Skills
(`agentskills.io <https://agentskills.io/>`_) over MCP. A skill is a directory
with a ``SKILL.md`` file and any supporting files. The server exposes each file
as a resource, conventionally ``skill://<skill-path>/<file-path>``, and the
standard ``resources/read`` reads it. The extension adds:

* ``skills/list`` and ``skills/get``, which return a skill's *entry*: the
  verbatim ``SKILL.md`` frontmatter and a complete list of the skill's files
  with the SHA-256 digest and size of each, or ``"resources": "dynamic"`` for a
  skill whose content is generated;
* ``resources/directory/read``, for servers that declare ``directoryRead``,
  which lists the direct children of a directory resource.

The format of a skill itself is the Agent Skills specification's. The SDK
implements the transport binding. The types follow the beta specification in
``ext-skills``, and the extension requires protocol revision 2026-07-28: the
methods are served to its clients only.

Using Agent Skills
------------------

A ``SkillsExtension`` serves the skills added to it, the same way in both
modes:

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :start-after: // Agent Skills.
   :end-before: // HTTP until a signal
   :dedent: 4

Serving skills
--------------

``mcp::skills::SkillsExtension`` declares the extension on a server and serves
the skills added to it as resources, declaring the ``resources`` capability when
the server has not:

.. code-block:: cpp

   #include <mcp/server/skills.hpp>

   mcp::Server server("docs", "1.0.0");

   mcp::skills::SkillsExtension skills(server);

   mcp::skills::SkillDefinition refunds;
   refunds.path = "acme/billing/refunds";
   refunds.frontmatter = {{"name", "refunds"},
                          {"description", "Process customer refund requests"}};
   refunds.files.push_back({"SKILL.md", skill_markdown, std::nullopt, {}});
   refunds.files.push_back({"examples/email.md", email_template, std::nullopt, {}});
   skills.add_skill(std::move(refunds));

Add skills before the server serves its first request. For each skill the
extension:

* checks the rules of the specification: the last segment of ``path`` is the
  skill's name and equals ``frontmatter.name`` and follows the Agent Skills
  naming rules, ``description`` is present, and the files include ``SKILL.md``
  and use plain relative paths;
* registers every file as a resource, with the MIME type the file's extension
  suggests unless the file sets one (text types are sent as ``text``, others as
  a base64 ``blob``), and names the ``SKILL.md`` resource after the skill;
* computes the digest and size of each file for the entry, and builds the
  directory resources for ``resources/directory/read``.

The frontmatter is published as written. The SDK does not parse YAML, so the
author keeps it identical to the frontmatter of the ``SKILL.md`` file; a host
compares the two before it loads the skill.

A file with a ``generate`` function is produced at each read. A skill with such
a file is listed with ``"resources": "dynamic"`` and no digests, and a host may
decline to load it.

``SkillsOptions`` sets ``directory_read`` (declare ``directoryRead`` and serve
``resources/directory/read``; on by default), the ``ttlMs`` and ``cacheScope``
of listings and entries, and the page size of ``skills/list`` and directory
reads.

Using skills from a client
--------------------------

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/client/skills.hpp>

   auto connected = co_await client.connect(info, capabilities);
   if (mcp::skills::supports_skills(connected.capabilities)) {
       auto listed = co_await mcp::skills::list_skills(client);
       auto entry = (co_await mcp::skills::get_skill(client, listed.skills[0].uri)).skill;

       // Verified against the size and digest in the entry:
       std::string text = co_await mcp::skills::read_skill_file(client, entry, entry.uri);
   }

``list_skills()`` and ``get_skill()`` reject an entry that breaks the rules the
specification puts on it (``validate_skill()``), including a listed file whose
path below the skill directory has an empty, ``.`` or ``..`` segment (also
percent-encoded), a backslash or a NUL. A listing may be empty or
partial without the server lacking skills, and ``get_skill()`` answers for any
skill the server serves. ``read_skill_file()`` reads a file with
``resources/read`` and throws when the entry does not list the URI, or when the
bytes differ from the listed size or digest; a host then refreshes the entry
with ``get_skill()``. A dynamic entry lists no files, so nothing is checked for
it. ``read_directory()`` sends ``resources/directory/read``
and refuses to do so unless the server declared ``directoryRead``
(``supports_directory_read()``).

What the SDK leaves to the host
-------------------------------

Digests are unsigned and come from the server that serves the content: a match
shows that the entry and the bytes agree, not that either is trustworthy. The
specification's security requirements fall on the host application and are not
implemented by the SDK: treating skill content as untrusted model input, tagging
it with its originating server, user approval bound to the entry's ``resources``
set, ignoring ``allowed-tools`` and other permission-widening frontmatter,
per-origin name resolution, and comparing the parsed ``SKILL.md`` frontmatter with
the entry's.
