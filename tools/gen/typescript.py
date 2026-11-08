"""Generates one ambient TypeScript declaration file for the script API."""
from .schema import pascal, waited_field


def doc(text, indent='', mark=None):
    lines = text.strip().replace('*/', '* /').splitlines()
    if mark and mark.deprecated: lines += [f'@deprecated Use {mark.use}.']
    return indent + '/**\n' + ''.join(indent + ' * ' + line + '\n' for line in lines) + indent + ' */\n'


def typename(t):
    if t.kind == 'handle': return pascal(t.name) + 'Handle'
    if t.kind == 'list': return typename(t.element) + '[]'
    if t.kind == 'flags': return t.name + '[]'
    if t.kind in ('enum', 'struct'): return t.name
    return {'bool': 'boolean', 'i32': 'number', 'u32': 'number', 'f32': 'number',
            'i64': 'bigint', 'u64': 'bigint', 'text': 'string', 'bytes': 'Uint8Array',
            'vec3': 'Vec3', 'rotation': 'Rotation', 'value': 'unknown', 'pointer': 'never',
            'callback': '(...args: any[]) => unknown'}[t.kind]


def declaration(schema):
    out = ['// Generated from schema/. Do not edit.\n', 'declare const tsuchiHandle: unique symbol;\n',
           'interface Subscription { cancel(): true; }\n',
           'interface Vec3 { x: number; y: number; z: number; }\n',
           'interface Rotation { pitch: number; yaw: number; roll: number; }\n']
    for handle in schema.handles:
        name, text = handle
        # Handle kinds are distinct; engine objects accept actor and component handles.
        kind = '"object" | "actor" | "component"' if name == 'object' else '"' + name + '"'
        out += [doc(text), f'interface {pascal(name)}Handle {{ readonly [tsuchiHandle]: {kind}; }}\n']
    for service in schema.services:
        if not service.has_bindings: continue
        for item in service.enums + service.flags:
            values = ' | '.join('"' + n + '"' for n, _, _ in item.values)
            if item.open: values += ' | (string & {})'
            else: values += ' | `#${number}`'
            value_docs = ''.join('\n' + n + ': ' + description for n, _, description in item.values)
            out += [doc(item.doc + value_docs, mark=item.mark), f'type {item.name} = {values};\n']
        for struct in service.structs:
            if struct.only == 'native': continue
            out += [doc(struct.doc, mark=struct.mark), f'interface {struct.name} {{\n']
            for f in struct.fields: out += [doc(f.doc, '    ', f.mark), f'    {f.name}{"?" if f.optional else ""}: {typename(f.type)}' + (' | null' if f.type.kind == 'handle' else '') + ';\n']
            out.append('}\n')
        for e in service.events:
            out += [doc(e.doc, mark=e.mark), f'interface {pascal(service.name)}{pascal(e.name)} {{\n']
            for f in e.fields: out += [doc(f.doc, '    ', f.mark), f'    {f.name}: {typename(f.type)}' + (' | null' if f.type.kind == 'handle' else '') + ';\n']
            out.append('}\n')
            waiter = next((fn for fn in service.functions if fn.waits == e.name and service.scripted(fn)), None)
            if waiter:
                field = waited_field(waiter, e)
                event_type = pascal(service.name) + pascal(e.name)
                out += [doc(f'What a call that waits for `{e.name}` returns: a Promise for the event, with the handle the call made as `{field.name}`.'),
                        f'interface {event_type}Promise extends Promise<{event_type}> {{ readonly {field.name}: {typename(field.type)}; }}\n']
        out += [doc(service.description, mark=service.mark), f'declare const {service.name}: {{\n']
        for fn in service.functions:
            if not service.scripted(fn): continue
            description = fn.doc
            for p in fn.params: description += '\n@param ' + p.name + ' ' + p.doc.replace('\n', ' ')
            if fn.returns: description += '\n@returns ' + fn.returns.doc.replace('\n', ' ')
            for code, message in fn.fails.items(): description += '\n@throws ' + code + ': ' + message
            params = []
            for p in fn.params:
                optional = p.optional or p.has_default
                params.append(p.name + ('?' if optional else '') + ': ' + typename(p.type) + (' | null' if p.optional and p.type.kind == 'handle' else ''))
            if fn.returns and fn.returns.type.kind == 'bytes': params.append('most: bigint')
            result = typename(fn.returns.type) if fn.returns else 'true'
            if fn.returns and (fn.returns.none or fn.returns.type.kind == 'handle'): result += ' | null'
            if fn.returns_outputs: result = f'[{result}, Record<string, unknown>]'
            if fn.waits == 'result': result = f'Promise<{typename(fn.returns.type) if fn.returns else "true"}>'
            elif fn.waits: result = pascal(service.name) + pascal(fn.waits) + 'Promise'
            out += [doc(description, '    ', fn.mark), f'    {fn.name}({", ".join(params)}): {result};\n']
        for e in service.events:
            out += [doc(e.doc, '    ', e.mark), f'    on(event: "{e.name}", fn: (event: {pascal(service.name)}{pascal(e.name)}) => void | Promise<void>): Subscription;\n']
        out.append('};\n')
    out += ['declare function print(...values: unknown[]): void;\n', 'declare const console: { log(...values: unknown[]): void };\n']
    return ''.join(out)


def generate(schema):
    return {'javascript/tsuchi.d.ts': declaration(schema)}
