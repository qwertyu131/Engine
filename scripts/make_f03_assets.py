"""F3 loader test fixtures (plan F3 Task 2) under tests/assets/f03/.

Usage:
    python scripts/make_f03_assets.py            # (re)writes every fixture
    python scripts/make_f03_assets.py --check    # exit 1 if a committed fixture differs from what this writes

Layout:
    bad/       malformed / hostile files (Review Focus 1). Each must make the loader exit 2 with a message;
               bad/expected.json lists every fixture with the message substring the test requires.
    missing/   valid files with missing attributes / material (Review Focus 4) and extension handling.
    paths/     .gltf files with external .bin / .png in the same and in a sibling directory, non-ASCII names
               with spaces, URI-encoded (%20, UTF-8 percent escapes) (Review Focus 2).

Every fixture is one triangle: POSITION (0,0,0) (1,0,0) (0,1,0), optional NORMAL +Z, TEXCOORD_0 (0,0) (1,0) (0,1),
uint16 indices 0 1 2. Images are 2 x 2 RGBA PNGs written here (stdlib zlib; the bytes depend on the zlib build, so
--check compares decoded JSON / binary payloads, not the PNG deflate stream). Stdlib only.
"""
from __future__ import annotations

import argparse
import base64
import json
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "tests" / "assets" / "f03"

POSITIONS = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
NORMALS = [(0.0, 0.0, 1.0)] * 3
UVS = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
INDICES = [0, 1, 2]


def png_bytes(w: int, h: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


PNG_2X2 = png_bytes(2, 2, bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]))


def f32(values) -> bytes:
    return b"".join(struct.pack("<f", v) for t in values for v in t)


def triangle_buffer(normals: bool = True, uvs: bool = True, index_values=INDICES) -> tuple[bytes, dict]:
    """Binary payload + the buffer views / accessors of the triangle (offsets into the payload)."""
    data = bytearray()
    views, accessors, attrs = [], [], {}

    def add(blob: bytes, target: int | None) -> int:
        while len(data) % 4:
            data.append(0)
        view = {"buffer": 0, "byteOffset": len(data), "byteLength": len(blob)}
        if target:
            view["target"] = target
        data.extend(blob)
        views.append(view)
        return len(views) - 1

    pv = add(f32(POSITIONS), 34962)
    accessors.append({"bufferView": pv, "componentType": 5126, "count": 3, "type": "VEC3",
                      "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 0.0]})
    attrs["POSITION"] = len(accessors) - 1
    if normals:
        nv = add(f32(NORMALS), 34962)
        accessors.append({"bufferView": nv, "componentType": 5126, "count": 3, "type": "VEC3"})
        attrs["NORMAL"] = len(accessors) - 1
    if uvs:
        uv = add(f32(UVS), 34962)
        accessors.append({"bufferView": uv, "componentType": 5126, "count": 3, "type": "VEC2"})
        attrs["TEXCOORD_0"] = len(accessors) - 1
    iv = add(b"".join(struct.pack("<H", i) for i in index_values), 34963)
    accessors.append({"bufferView": iv, "componentType": 5123, "count": len(index_values), "type": "SCALAR"})
    while len(data) % 4:
        data.append(0)
    return bytes(data), {"bufferViews": views, "accessors": accessors, "attributes": attrs,
                         "indices": len(accessors) - 1}


def gltf_doc(layout: dict, buffer_uri: str | None, buffer_len: int, material: dict | None = None,
             images: list | None = None, extra: dict | None = None) -> dict:
    prim = {"attributes": layout["attributes"], "indices": layout["indices"]}
    doc: dict = {"asset": {"version": "2.0", "generator": "scripts/make_f03_assets.py"},
                 "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
                 "meshes": [{"primitives": [prim]}],
                 "buffers": [{"byteLength": buffer_len}],
                 "bufferViews": layout["bufferViews"], "accessors": layout["accessors"]}
    if buffer_uri is not None:
        doc["buffers"][0]["uri"] = buffer_uri
    if material is not None:
        doc["materials"] = [material]
        prim["material"] = 0
    if images:
        doc["images"] = images
        doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 33071}]
        doc["textures"] = [{"source": i, "sampler": 0} for i in range(len(images))]
    if extra:
        doc.update(extra)
    return doc


def data_uri(mime: str, payload: bytes) -> str:
    return f"data:{mime};base64," + base64.b64encode(payload).decode("ascii")


def dumps(doc: dict) -> bytes:
    return (json.dumps(doc, indent=1, ensure_ascii=False) + "\n").encode("utf-8")


def glb(doc: dict, bin_chunk: bytes) -> bytes:
    j = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    j += b" " * ((4 - len(j) % 4) % 4)
    b = bin_chunk + b"\0" * ((4 - len(bin_chunk) % 4) % 4)
    total = 12 + 8 + len(j) + 8 + len(b)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(j), b"JSON") + j +
            struct.pack("<I4s", len(b), b"BIN\0") + b)


def fixtures() -> dict[str, bytes]:
    """Relative path under tests/assets/f03 -> file bytes."""
    files: dict[str, bytes] = {}
    expected: list[dict] = []

    def bad(name: str, data: bytes, message: str, focus: str) -> None:
        files[f"bad/{name}"] = data
        expected.append({"file": name, "message": message, "reviewFocus": focus})

    tri, layout = triangle_buffer()
    embedded = data_uri("application/octet-stream", tri)

    # --- bad/ (Review Focus 1) ---------------------------------------------------------------------------
    good_glb = glb(gltf_doc(layout, None, len(tri)), tri)
    bad("truncated.glb", good_glb[:len(good_glb) - 20], "truncated", "truncated .glb")
    # GLB whose BIN chunk claims more bytes than the file holds (header length patched to match the file).
    g = bytearray(good_glb)
    jlen = struct.unpack_from("<I", g, 12)[0]
    struct.pack_into("<I", g, 20 + jlen, 0x7FFFFFF0)
    bad("chunk_overflow.glb", bytes(g), "GLB chunk", "truncated .glb (chunk length past the end)")

    acc = json.loads(json.dumps(layout))
    acc["accessors"][0]["count"] = 100  # 100 * 12 bytes > the 36-byte position view
    bad("accessor_out_of_range.gltf", dumps(gltf_doc(acc, embedded, len(tri))), "accessor 0",
        "accessor out of buffer range")

    acc = json.loads(json.dumps(layout))
    acc["accessors"][0]["count"] = 2 ** 31
    bad("accessor_count_2pow31.gltf", dumps(gltf_doc(acc, embedded, len(tri))), "accessor 0",
        "a 2^31 accessor count")

    acc = json.loads(json.dumps(layout))
    acc["bufferViews"][0]["byteLength"] = 4096  # view past the end of the buffer
    bad("bufferview_out_of_range.gltf", dumps(gltf_doc(acc, embedded, len(tri))), "bufferView 0",
        "accessor out of buffer range (via its bufferView)")

    tri_bad_index, layout_bi = triangle_buffer(index_values=[0, 1, 7])
    bad("index_out_of_range.gltf",
        dumps(gltf_doc(layout_bi, data_uri("application/octet-stream", tri_bad_index), len(tri_bad_index))),
        "index 7 >= vertex count 3", "index >= vertex count")

    bad("bad_base64.gltf", dumps(gltf_doc(layout, "data:application/octet-stream;base64," + "@@@@" * 20,
                                          len(tri))), "buffer 0", "a data-URI with bad base64")

    not_png = b"this is not a PNG file, just text" + bytes(range(32))
    mat = {"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}
    bad("bad_image.gltf", dumps(gltf_doc(layout, embedded, len(tri), mat, [{"uri": data_uri("image/png", not_png)}])),
        "image 0", "an image that fails to decode")

    sp = json.loads(json.dumps(layout))
    # Sparse accessor: 1 substituted element at index 9 of a 3-element accessor.
    sparse_blob = struct.pack("<H", 9) + b"\0\0" + struct.pack("<3f", 5.0, 5.0, 5.0)
    blob = tri + sparse_blob
    sp["bufferViews"].append({"buffer": 0, "byteOffset": len(tri), "byteLength": 4})
    sp["bufferViews"].append({"buffer": 0, "byteOffset": len(tri) + 4, "byteLength": 12})
    sp["accessors"][0]["sparse"] = {"count": 1,
                                    "indices": {"bufferView": len(sp["bufferViews"]) - 2, "componentType": 5123},
                                    "values": {"bufferView": len(sp["bufferViews"]) - 1}}
    bad("sparse_index_out_of_range.gltf",
        dumps(gltf_doc(sp, data_uri("application/octet-stream", blob), len(blob))), "sparse",
        "accessor out of range (sparse index)")

    ub = json.loads(json.dumps(layout))
    ub["accessors"][layout["indices"]] = {"componentType": 5121, "count": 2 ** 31, "type": "SCALAR"}
    bad("unbacked_accessor_2pow31.gltf", dumps(gltf_doc(ub, embedded, len(tri))), "without a bufferView",
        "a 2^31 accessor count without a bufferView (zeros: nothing to range-check against)")

    cyc = gltf_doc(layout, embedded, len(tri))
    cyc["nodes"] = [{"mesh": 0}, {"children": [2]}, {"children": [1]}]  # 1 -> 2 -> 1, one parent each
    bad("node_cycle.gltf", dumps(cyc), "cycle", "node hierarchy with a cycle (hang guard)")

    bad("missing_buffer_file.gltf", dumps(gltf_doc(layout, "does_not_exist.bin", len(tri))), "does_not_exist.bin",
        "external buffer missing")
    bad("not_json.gltf", b"{ this is not json ]\n", "JSON", "malformed JSON")
    bad("unknown_required_extension.gltf",
        dumps(gltf_doc(layout, embedded, len(tri), extra={"extensionsUsed": ["EXT_does_not_exist"],
                                                          "extensionsRequired": ["EXT_does_not_exist"]})),
        "unsupported required extension EXT_does_not_exist", "spec 7.2 unknown required extension")
    bad("later_phase_required_extension.gltf",
        dumps(gltf_doc(layout, embedded, len(tri),
                       {"extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1.0}}},
                       extra={"extensionsUsed": ["KHR_materials_clearcoat"],
                              "extensionsRequired": ["KHR_materials_clearcoat"]})),
        "KHR_materials_clearcoat is supported from F6", "spec 7.2 later-phase required extension")
    files["bad/expected.json"] = (json.dumps({"comment": "Generated by scripts/make_f03_assets.py: every bad/ fixture "
                                              "and the substring of the exit-2 message the loader must give.",
                                              "fixtures": expected}, indent=1) + "\n").encode("utf-8")

    # --- missing/ (Review Focus 4) + extension handling -----------------------------------------------------
    t_nn, l_nn = triangle_buffer(normals=False)
    files["missing/no_normals.gltf"] = dumps(gltf_doc(l_nn, data_uri("application/octet-stream", t_nn), len(t_nn)))
    normal_mat = {"normalTexture": {"index": 0}}
    files["missing/no_tangents_normal_map.gltf"] = dumps(
        gltf_doc(layout, embedded, len(tri), normal_mat, [{"uri": data_uri("image/png", PNG_2X2)}]))
    t_nu, l_nu = triangle_buffer(uvs=False)
    files["missing/no_uv_with_texture.gltf"] = dumps(
        gltf_doc(l_nu, data_uri("application/octet-stream", t_nu), len(t_nu),
                 {"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
                 [{"uri": data_uri("image/png", PNG_2X2)}]))
    # Non-finite POSITION / UV with a normal map and no TANGENT: MikkTSpace and the flat-normal path must return.
    nan, inf = float("nan"), float("inf")
    blob = bytearray(tri)
    struct.pack_into("<3f", blob, 12, nan, inf, 0.0)  # vertex 1 position
    struct.pack_into("<2f", blob, layout["bufferViews"][2]["byteOffset"] + 8, nan, -inf)  # vertex 1 uv
    files["missing/nonfinite_tangent_input.gltf"] = dumps(
        gltf_doc(layout, data_uri("application/octet-stream", bytes(blob)), len(blob), normal_mat,
                 [{"uri": data_uri("image/png", PNG_2X2)}]))
    files["missing/no_material.gltf"] = dumps(gltf_doc(layout, embedded, len(tri)))
    files["missing/unknown_used_extension.gltf"] = dumps(
        gltf_doc(layout, embedded, len(tri), extra={"extensionsUsed": ["EXT_does_not_exist"]}))
    files["missing/later_phase_used_extension.gltf"] = dumps(
        gltf_doc(layout, embedded, len(tri),
                 {"extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 0.5}}},
                 extra={"extensionsUsed": ["KHR_materials_clearcoat"]}))

    # --- paths/ (Review Focus 2) ---------------------------------------------------------------------------
    mat = {"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}
    same = "çıktı klasörü"
    files[f"paths/{same}/model bin ü.bin"] = tri
    files[f"paths/{same}/doku ç.png"] = PNG_2X2
    files[f"paths/{same}/model with spaces.gltf"] = dumps(
        gltf_doc(layout, "model%20bin%20%C3%BC.bin", len(tri), mat, [{"uri": "doku%20%C3%A7.png"}]))
    # Raw (not percent-encoded) UTF-8 + space in the URI: accepted by viewers, as fastgltf documents.
    files[f"paths/{same}/raw utf8 uri.gltf"] = dumps(
        gltf_doc(layout, "model bin ü.bin", len(tri), mat, [{"uri": "doku ç.png"}]))
    files["paths/kaynak ğ/geometri ş.bin"] = tri
    files["paths/kaynak ğ/resim ö.png"] = PNG_2X2
    files["paths/sahne ı/sibling dir.gltf"] = dumps(
        gltf_doc(layout, "../kaynak%20%C4%9F/geometri%20%C5%9F.bin", len(tri), mat,
                 [{"uri": "../kaynak%20%C4%9F/resim%20%C3%B6.png"}]))
    return files


def payload(rel: str, data: bytes):
    """What --check compares: PNG pixels via zlib-decompressed IDAT (deflate bytes vary by zlib build)."""
    if rel.endswith(".png"):
        idat, pos = b"", 8
        while pos < len(data):
            (n,) = struct.unpack(">I", data[pos:pos + 4])
            if data[pos + 4:pos + 8] == b"IDAT":
                idat += data[pos + 8:pos + 8 + n]
            pos += 12 + n
        return zlib.decompress(idat)
    return data


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if a committed fixture is stale or missing")
    args = ap.parse_args(argv)
    files = fixtures()
    if args.check:
        stale = []
        for rel, data in files.items():
            p = OUT / rel
            if not p.is_file() or payload(rel, p.read_bytes()) != payload(rel, data):
                stale.append(rel)
        if stale:
            print("make_f03_assets: stale or missing: " + ", ".join(stale) +
                  " (run python scripts/make_f03_assets.py)", file=sys.stderr)
            return 1
        print(f"make_f03_assets: {len(files)} fixtures up to date")
        return 0
    for rel, data in files.items():
        p = OUT / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    print(f"make_f03_assets: wrote {len(files)} files under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
