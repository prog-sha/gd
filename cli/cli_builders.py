"""Embed scripts, manuals, and API purpose descriptions as C++ arrays."""

import json

import methods


def make_pkg_script(target, source, env):
    """Convert cli/tool/pkg.gd into the byte array in pkg.gen.h."""
    buffer = methods.get_buffer(str(source[0]))

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(f"""\
inline constexpr const unsigned char gd_pkg_script[] = {{
{methods.format_buffer(buffer, 1)}
}};
""")


def make_manual(target, source, env):
    """Embed manual byte arrays with Japanese then English source ordering."""
    with methods.generated_wrapper(str(target[0])) as file:
        for name, path in zip(("ja", "en"), source):
            file.write(f"""\
inline constexpr const unsigned char gd_manual_{name}[] = {{
{methods.format_buffer(methods.get_buffer(str(path)), 1)}
}};

""")


def make_briefs(target, source, env):
    """Build a terminal lookup table from the two API-description language editions."""
    with open(str(source[0]), encoding="utf-8") as file:
        ja = json.load(file)
    with open(str(source[1]), encoding="utf-8") as file:
        en = json.load(file)
    if set(ja) != set(en):
        raise ValueError("docs/api.json と docs/api.en.json の名前が一致しない")

    rows = "".join(
        f"\t{{ {json.dumps(name)}, {json.dumps(text, ensure_ascii=False)}, {json.dumps(en[name], ensure_ascii=False)} }},\n"
        for name, text in ja.items()
    )

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(f"""\
struct GDBrief {{
\tconst char *name; // API page name.
\tconst char *ja; // One-line purpose description in Japanese.
\tconst char *en; // Corresponding purpose description in English.
}};

inline constexpr GDBrief gd_briefs[] = {{
{rows}}};
""")
