"""F3 loader test fixtures (plan F3 Task 2) under tests/assets/f03/.

Usage:
    python scripts/make_f03_assets.py            # (re)writes every fixture
    python scripts/make_f03_assets.py --check    # exit 1 if a committed fixture differs from what this writes

Layout:
    bad/       malformed / hostile files (Review Focus 1). Each must make the loader exit 2 with a message;
               bad/expected.json lists every fixture with the message substring the test requires.
    missing/   valid files with missing attributes / material (Review Focus 4) and extension handling.
    colorspace/ the crafted TextureLinearInterpolationTest variant of the Review Focus 3 colour-space probe (Task 3).
    alpha/     MASK and BLEND quads over the bare background (Task 4 facade probes).
    rest/      a quad displaced +2 in x by a skin or by a morph target at rest pose (Task 4 facade probes).
    paths/     .gltf files with external .bin / .png in the same and in a sibling directory, non-ASCII names
               with spaces, URI-encoded (%20, UTF-8 percent escapes) (Review Focus 2).

The decoder fixtures of bad/ (plan F3 Task 3) are cut from small glTF-Sample-Assets files (Box/glTF-Draco,
MeshoptCubeTest/glTF-Meshopt, CarConcept KTX2 / WebP images), so writing or checking them needs the Sample-Assets
checkout (scripts/fetch_assets.py). Every other fixture is one triangle: POSITION (0,0,0) (1,0,0) (0,1,0), optional NORMAL +Z, TEXCOORD_0 (0,0) (1,0) (0,1),
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
    # --- bad/ decoder inputs (plan F3 Task 3, Review Focus 1): built from small Sample-Assets files --------------------
    sample = REPO / "tests" / "assets" / "glTF-Sample-Assets" / "Models"
    if not sample.is_dir():
        raise SystemExit(f"make_f03_assets: {sample} missing (run python scripts/fetch_assets.py)")
    box = json.loads((sample / "Box" / "glTF-Draco" / "Box.gltf").read_text(encoding="utf-8"))
    box_bin = (sample / "Box" / "glTF-Draco" / "Box.bin").read_bytes()
    box["buffers"][0]["uri"] = data_uri("application/octet-stream", box_bin)
    box["asset"]["generator"] = "scripts/make_f03_assets.py (from glTF-Sample-Assets Box/glTF-Draco)"
    d = json.loads(json.dumps(box))
    d["bufferViews"][0]["byteLength"] = 40  # 118-byte Draco stream cut to 40 bytes
    bad("draco_truncated.gltf", dumps(d), "Draco", "truncated Draco stream (KHR_draco_mesh_compression)")
    d = json.loads(json.dumps(box))
    d["accessors"][2]["count"] = 23  # POSITION accessor: the stream decodes 24 points
    bad("draco_count_mismatch.gltf", dumps(d), "decoded Draco points",
        "Draco point count != accessor count")

    cube = json.loads((sample / "MeshoptCubeTest" / "glTF-Meshopt" / "MeshoptCubeTest.gltf").read_text(encoding="utf-8"))
    cube_bin = (sample / "MeshoptCubeTest" / "glTF-Meshopt" / "MeshoptCubeTest.bin").read_bytes()
    for key in ("images", "textures", "samplers", "materials", "animations"):
        cube.pop(key, None)
    for mesh in cube["meshes"]:
        for prim in mesh["primitives"]:
            prim.pop("material", None)
    cube["buffers"][0]["uri"] = data_uri("application/octet-stream", cube_bin)
    cube["asset"]["generator"] = "scripts/make_f03_assets.py (from glTF-Sample-Assets MeshoptCubeTest/glTF-Meshopt)"
    first = next(i for i, v in enumerate(cube["bufferViews"]) if "KHR_meshopt_compression" in v.get("extensions", {}))
    d = json.loads(json.dumps(cube))
    ext = d["bufferViews"][first]["extensions"]["KHR_meshopt_compression"]
    ext["byteLength"] = max(1, ext["byteLength"] // 2)  # compressed stream cut in half
    bad("meshopt_truncated.gltf", dumps(d), "meshopt", "truncated meshopt stream (KHR_meshopt_compression)")
    d = json.loads(json.dumps(cube))
    attr = next(i for i, v in enumerate(d["bufferViews"])
                if v.get("extensions", {}).get("KHR_meshopt_compression", {}).get("mode") == "ATTRIBUTES")
    d["bufferViews"][attr]["extensions"]["KHR_meshopt_compression"]["byteStride"] = 6  # not a multiple of 4
    bad("meshopt_bad_stride.gltf", dumps(d), "byteStride", "meshopt ATTRIBUTES stride the codec does not allow")
    # Unbounded inputs capped by r.Asset.MaxFileBytes (default 2 GiB): the decoded size of a compressed view comes from
    # the file, not from bytes in it. 3 GiB = just above the default cap (a loader without the cap allocates it).
    over_cap = 3 << 30
    d = json.loads(json.dumps(cube))
    d["bufferViews"][first]["byteLength"] = over_cap
    bad("meshopt_huge_view.gltf", dumps(d), "declared byteLength 3221225472 exceeds r.Asset.MaxFileBytes",
        "meshopt view whose declared (decoded) byteLength is above r.Asset.MaxFileBytes")
    d = json.loads(json.dumps(cube))
    fallback = next(i for i, b in enumerate(d["buffers"])
                    if b.get("extensions", {}).get("KHR_meshopt_compression", {}).get("fallback"))
    d["buffers"][fallback]["byteLength"] = over_cap
    bad("meshopt_huge_fallback.gltf", dumps(d), "fallback buffer byteLength 3221225472 exceeds r.Asset.MaxFileBytes",
        "meshopt fallback buffer (no data) whose declared byteLength is above r.Asset.MaxFileBytes")
    # Two stride-20 ATTRIBUTES views of 1.5 GiB each (each under the cap) over one 1.5 GiB fallback buffer: together
    # they decode to 3 GiB, above the cap; refused before any of it is allocated.
    d = json.loads(json.dumps(cube))
    d["buffers"][fallback]["byteLength"] = 3 << 29
    big = [i for i, v in enumerate(d["bufferViews"])
           if v.get("extensions", {}).get("KHR_meshopt_compression", {}).get("byteStride") == 20][:2]
    for i in big:
        v = d["bufferViews"][i]
        v["byteOffset"] = 0
        v["byteLength"] = 3 << 29
        v["extensions"]["KHR_meshopt_compression"]["count"] = (3 << 29) // 20
    bad("meshopt_total_decoded.gltf", dumps(d), "meshopt views decode to more than r.Asset.MaxFileBytes",
        "many meshopt views, each under r.Asset.MaxFileBytes, whose decoded total is above it")

    def tex_ext_doc(ext_name: str, mime: str, payload: bytes) -> bytes:
        doc = gltf_doc(layout, embedded, len(tri), {"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}})
        doc["images"] = [{"uri": data_uri(mime, payload)}]
        doc["textures"] = [{"extensions": {ext_name: {"source": 0}}}]
        doc["extensionsUsed"] = [ext_name]
        doc["extensionsRequired"] = [ext_name]
        return dumps(doc)

    ktx2 = (sample / "CarConcept" / "glTF-KTX-BasisU-Draco" / "Rib_N.ktx2").read_bytes()
    bad("ktx2_truncated.gltf", tex_ext_doc("KHR_texture_basisu", "image/ktx2", ktx2[:len(ktx2) // 2]), "image/ktx2",
        "truncated KTX2 image (KHR_texture_basisu)")
    webp = (sample / "CarConcept" / "glTF-WEBP" / "Hex_N.webp").read_bytes()
    bad("webp_truncated.gltf", tex_ext_doc("EXT_texture_webp", "image/webp", webp[:len(webp) // 2]), "WebP",
        "truncated WebP image (EXT_texture_webp)")

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

    # --- colorspace/ (Review Focus 3, plan F3 Task 3) --------------------------------------------------------------
    # TextureLinearInterpolationTest's 2 x 1 emissive texture is (0, 0, 0) - (0, 255, 0): both texels are fixed points
    # of the sRGB curve, so an sRGB <-> linear view swap leaves its midpoint at 0.5 and no probe of it can see the swap.
    # The crafted variant keeps the asset's design (a 2 x 1 texture sampled midway, bilinear) with a second texel of
    # (0, 188, 0): sRGB view -> midpoint (0 + srgbToLinear(188 / 255)) / 2 = 0.251; UNORM view -> 0.369. The same PNG is
    # also used as an occlusion (linear) texture through its own image, so both directions of the swap are probed.
    px = bytes([0, 0, 0, 255, 0, 188, 0, 255])
    png_2x1 = png_bytes(2, 1, px)
    cs = gltf_doc(layout, embedded, len(tri), {"emissiveTexture": {"index": 0}, "emissiveFactor": [1.0, 1.0, 1.0]},
                  [{"uri": data_uri("image/png", png_2x1)}, {"uri": data_uri("image/png", png_2x1)}])
    cs["materials"].append({"occlusionTexture": {"index": 1}})
    cs["samplers"] = [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}]
    cs["meshes"].append({"primitives": [{"attributes": layout["attributes"], "indices": layout["indices"],
                                         "material": 1}]})
    cs["nodes"].append({"mesh": 1, "translation": [2.0, 0.0, 0.0]})
    cs["scenes"][0]["nodes"] = [0, 1]
    files["colorspace/linear_interpolation_crafted.gltf"] = dumps(cs)

    # --- alpha/ (plan F3 Task 4): MASK and BLEND over the bare background ------------------------------------------
    # AlphaBlendModeTest's quads stand in front of its marble bed, so a cut-out there shows the bed, not the background.
    # This fixture puts the two modes over nothing: quad A (x -1.1 .. -0.1) is MASK (cutoff 0.5) with a 2 x 1 NEAREST
    # texture whose left texel has alpha 0 and right texel alpha 255 (both green): its left half is cut away, its right
    # half drawn; quad B (x 0.1 .. 1.1) is BLEND with baseColorFactor (0, 0, 1, 0.5), no texture: half transparent.
    # Both face +Z, metallic 0 / roughness 1, single-sided.
    quad_pos = [(-1.1, -0.5, 0.0), (-0.1, -0.5, 0.0), (-0.1, 0.5, 0.0), (-1.1, 0.5, 0.0),
                (0.1, -0.5, 0.0), (1.1, -0.5, 0.0), (1.1, 0.5, 0.0), (0.1, 0.5, 0.0)]
    quad_nrm = [(0.0, 0.0, 1.0)] * 8
    quad_uv = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)] * 2
    quad_idx = [0, 1, 2, 0, 2, 3] * 2  # each quad indexes its own 4 vertices
    blob = f32(quad_pos) + f32(quad_nrm) + f32(quad_uv) + b"".join(struct.pack("<H", i) for i in quad_idx)
    n_off, uv_off, i_off = 96, 192, 256
    alpha_doc = {
        "asset": {"version": "2.0", "generator": "scripts/make_f03_assets.py"},
        "scene": 0, "scenes": [{"nodes": [0, 1]}], "nodes": [{"mesh": 0}, {"mesh": 1}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3,
                                    "material": 0}]},
                   {"primitives": [{"attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6}, "indices": 7,
                                    "material": 1}]}],
        "materials": [{"alphaMode": "MASK", "alphaCutoff": 0.5,
                       "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0,
                                                "roughnessFactor": 1.0}},
                      {"alphaMode": "BLEND",
                       "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 1.0, 0.5], "metallicFactor": 0.0,
                                                "roughnessFactor": 1.0}}],
        "images": [{"uri": data_uri("image/png", png_bytes(2, 1, bytes([0, 255, 0, 0, 0, 255, 0, 255])))}],
        "samplers": [{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": 0, "sampler": 0}],
        "buffers": [{"byteLength": len(blob), "uri": data_uri("application/octet-stream", blob)}],
        # Vertex views shared by both quads' accessors: byteStride is required then (glTF 2.0 §3.6.2.3).
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 12},
                        {"buffer": 0, "byteOffset": n_off, "byteLength": 96, "byteStride": 12},
                        {"buffer": 0, "byteOffset": uv_off, "byteLength": 64, "byteStride": 8},
                        {"buffer": 0, "byteOffset": i_off, "byteLength": 24}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-1.1, -0.5, 0.0], "max": [-0.1, 0.5, 0.0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
            {"bufferView": 0, "byteOffset": 48, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [0.1, -0.5, 0.0], "max": [1.1, 0.5, 0.0]},
            {"bufferView": 1, "byteOffset": 48, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "byteOffset": 32, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "byteOffset": 12, "componentType": 5123, "count": 6, "type": "SCALAR"}]}
    files["alpha/mask_and_blend.gltf"] = dumps(alpha_doc)

    # --- rest/ (plan F3 Task 4): skinning and morphing at rest pose, with an exact displacement -------------------
    # A unit quad (x, y in [-0.5, 0.5], z 0, facing +Z) displaced +2 in x two ways: skin_quad.gltf skins every vertex
    # 100 % to joint node 1 (translation (2, 0, 0), no inverseBindMatrices = identity); morph_quad.gltf has one morph
    # target with POSITION deltas (+4, 0, 0) and mesh.weights [0.5]. Either way the quad is drawn around (2, 0, 0)
    # and nothing at the origin.
    def quad_doc(extra_attrs: bytes, attrs: dict, extra_views: list, extra_accessors: list, mesh_extra: dict,
                 nodes: list, scene_nodes: list, more: dict) -> bytes:
        pos = [(-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.5, 0.5, 0.0), (-0.5, 0.5, 0.0)]
        nrm = [(0.0, 0.0, 1.0)] * 4
        idx = [0, 1, 2, 0, 2, 3]
        base = f32(pos) + f32(nrm) + b"".join(struct.pack("<H", i) for i in idx) + b"\0\0\0\0"  # 4-aligned
        data = base + extra_attrs
        views = [{"buffer": 0, "byteOffset": 0, "byteLength": 48}, {"buffer": 0, "byteOffset": 48, "byteLength": 48},
                 {"buffer": 0, "byteOffset": 96, "byteLength": 12}]
        accs = [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-0.5, -0.5, 0.0],
                 "max": [0.5, 0.5, 0.0]},
                {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
                {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}]
        for v in extra_views:
            v = dict(v)
            v["byteOffset"] += len(base)
            views.append(v)
        accs += extra_accessors
        prim = {"attributes": {"POSITION": 0, "NORMAL": 1, **attrs}, "indices": 2, "material": 0}
        prim.update(mesh_extra.get("primitive", {}))
        mesh = {"primitives": [prim]}
        mesh.update(mesh_extra.get("mesh", {}))
        doc = {"asset": {"version": "2.0", "generator": "scripts/make_f03_assets.py"}, "scene": 0,
               "scenes": [{"nodes": scene_nodes}], "nodes": nodes, "meshes": [mesh],
               "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.9, 0.9, 1.0], "metallicFactor": 0.0,
                                                       "roughnessFactor": 1.0}}],
               "buffers": [{"byteLength": len(data), "uri": data_uri("application/octet-stream", data)}],
               "bufferViews": views, "accessors": accs}
        doc.update(more)
        return dumps(doc)

    joints = b"".join(struct.pack("<4B", 0, 0, 0, 0) for _ in range(4))
    weights = f32([(1.0, 0.0, 0.0, 0.0)] * 4)
    files["rest/skin_quad.gltf"] = quad_doc(
        joints + weights, {"JOINTS_0": 3, "WEIGHTS_0": 4},
        [{"buffer": 0, "byteOffset": 0, "byteLength": 16}, {"buffer": 0, "byteOffset": 16, "byteLength": 64}],
        [{"bufferView": 3, "componentType": 5121, "count": 4, "type": "VEC4"},
         {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC4"}],
        {}, [{"mesh": 0, "skin": 0}, {"translation": [2.0, 0.0, 0.0]}], [0, 1], {"skins": [{"joints": [1]}]})
    deltas = f32([(4.0, 0.0, 0.0)] * 4)
    files["rest/morph_quad.gltf"] = quad_doc(
        deltas, {}, [{"buffer": 0, "byteOffset": 0, "byteLength": 48}],
        [{"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3", "min": [4.0, 0.0, 0.0],
          "max": [4.0, 0.0, 0.0]}],
        {"primitive": {"targets": [{"POSITION": 3}]}, "mesh": {"weights": [0.5]}}, [{"mesh": 0}], [0], {})

    # --- colorspace/emissive_view.gltf (Review Focus 3 through the renderer's slot -> view table) ---------------------
    # Two quads, black base colour, metallic 0, roughness 1: quad A (x -1.1 .. -0.1) has emissiveFactor
    # (0, srgbToLinear(188 / 255) / 2, 0) and no texture; quad B (x 0.1 .. 1.1) has emissiveFactor (1, 1, 1) and the
    # crafted 2 x 1 emissive texture (0, 0, 0) - (0, 188, 0) sampled at its midpoint (every UV = (0.5, 0.5), LINEAR).
    # With emissive as an sRGB view both quads emit the same green; a linear view makes B brighter.
    g = ((188 / 255 + 0.055) / 1.055) ** 2.4 / 2
    ev_pos = [(-1.1, -0.5, 0.0), (-0.1, -0.5, 0.0), (-0.1, 0.5, 0.0), (-1.1, 0.5, 0.0),
              (0.1, -0.5, 0.0), (1.1, -0.5, 0.0), (1.1, 0.5, 0.0), (0.1, 0.5, 0.0)]
    ev_blob = (f32(ev_pos) + f32([(0.0, 0.0, 1.0)] * 8) + f32([(0.5, 0.5)] * 8) +
               b"".join(struct.pack("<H", i) for i in [0, 1, 2, 0, 2, 3] * 2))
    black = {"baseColorFactor": [0.0, 0.0, 0.0, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}
    ev_doc = {
        "asset": {"version": "2.0", "generator": "scripts/make_f03_assets.py"},
        "scene": 0, "scenes": [{"nodes": [0, 1]}], "nodes": [{"mesh": 0}, {"mesh": 1}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3,
                                    "material": 0}]},
                   {"primitives": [{"attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6}, "indices": 7,
                                    "material": 1}]}],
        "materials": [{"emissiveFactor": [0.0, g, 0.0], "pbrMetallicRoughness": black},
                      {"emissiveFactor": [1.0, 1.0, 1.0], "emissiveTexture": {"index": 0}, "pbrMetallicRoughness": black}],
        "images": [{"uri": data_uri("image/png", png_2x1)}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": 0, "sampler": 0}],
        "buffers": [{"byteLength": len(ev_blob), "uri": data_uri("application/octet-stream", ev_blob)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 12},
                        {"buffer": 0, "byteOffset": 96, "byteLength": 96, "byteStride": 12},
                        {"buffer": 0, "byteOffset": 192, "byteLength": 64, "byteStride": 8},
                        {"buffer": 0, "byteOffset": 256, "byteLength": 24}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1.1, -0.5, 0.0], "max": [-0.1, 0.5, 0.0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
            {"bufferView": 0, "byteOffset": 48, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0.1, -0.5, 0.0], "max": [1.1, 0.5, 0.0]},
            {"bufferView": 1, "byteOffset": 48, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "byteOffset": 32, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "byteOffset": 12, "componentType": 5123, "count": 6, "type": "SCALAR"}]}
    files["colorspace/emissive_view.gltf"] = dumps(ev_doc)

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
