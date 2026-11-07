"""Generators that turn the API schema into headers, bindings and reference pages."""
from . import c_header, docs_page, game_table, js_binding, lua_binding, service_glue, typescript
from .schema import SchemaError, load

GENERATORS = (c_header, service_glue, lua_binding, js_binding, typescript, docs_page)


def generate_all(schema_folder, table=None):
    """{relative path: text} for everything generated from the schema and, when given, the game table."""
    schema = load(schema_folder)
    if table:
        schema.call_families = game_table.call_families(table, schema)
    files = {}
    for generator in GENERATORS:
        files.update(generator.generate(schema))
    if table:
        files.update(game_table.generate(table, 'game/table.yaml'))
    return files
