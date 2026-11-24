"""One-off converter: Windhawk mod settings (YAML block in the .wh.cpp) ->
src/shared/settings_schema.json. After the migration the JSON is the source of
truth; this script is kept only to show where the schema came from."""
import json, sys, yaml

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
a = text.index("// ==WindhawkModSettings==")
b = text.index("// ==/WindhawkModSettings==")
data = yaml.safe_load(text[a:b].split("/*", 1)[1].rsplit("*/", 1)[0])

groups, settings = [], []
for g in data:
    gkey = [k for k in g if not k.startswith("$")][0]
    groups.append({"key": gkey, "name": g.get("$name", gkey)})
    for item in g[gkey]:
        key = [k for k in item if not k.startswith("$")][0]
        default = item[key]
        entry = {"key": f"{gkey}.{key}", "group": gkey, "name": item.get("$name", key)}
        if "$description" in item:
            entry["description"] = item["$description"]
        if "$options" in item:
            entry["type"] = "enum"
            entry["options"] = [{"value": str(list(o)[0]), "label": str(list(o.values())[0])} for o in item["$options"]]
            entry["default"] = str(default)
        elif isinstance(default, bool):
            entry["type"] = "bool"; entry["default"] = default
        elif isinstance(default, int):
            entry["type"] = "int"; entry["default"] = default
        elif isinstance(default, str):
            entry["type"] = "string"; entry["default"] = default
        else:
            raise SystemExit(f"unsupported default for {gkey}.{key}: {default!r}")
        settings.append(entry)

out = {"schemaVersion": 1, "groups": groups, "settings": settings}
json.dump(out, open(dst, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
print(len(groups), "groups,", len(settings), "settings")
