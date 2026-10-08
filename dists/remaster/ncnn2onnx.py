"""Convert the Real-ESRGAN animevideov3 ncnn model (.param/.bin) to ONNX with the same weights.

The ONNX graph takes and returns RGBA bytes ([1, H, W, 4] uint8 -> [1, 3H, 3W, 4] uint8), so only bytes cross
between CPU and GPU. Usage: ncnn2onnx.py <model base> <out.onnx> [fp16]
"""
import struct
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

base, out_path = sys.argv[1], sys.argv[2]
fp16 = "fp16" in sys.argv[3:]
add_x3 = "x3" in sys.argv[3:]

layers = []
with open(base + ".param") as f:
    assert f.readline().strip() == "7767517"
    f.readline()
    for line in f:
        parts = line.split()
        if not parts:
            continue
        typ, name, nin, nout = parts[0], parts[1], int(parts[2]), int(parts[3])
        ins = parts[4:4 + nin]
        outs = parts[4 + nin:4 + nin + nout]
        params = {}
        for kv in parts[4 + nin + nout:]:
            k, v = kv.split("=")
            params[int(k)] = [float(x) for x in v.split(",")[1:]] if int(k) <= -23300 else v
        layers.append((typ, name, ins, outs, params))

blob = open(base + ".bin", "rb").read()
pos = 0


def take_floats(n):
    global pos
    v = np.frombuffer(blob, dtype="<f4", count=n, offset=pos).copy()
    pos += 4 * n
    return v


def take_weights(n):
    global pos
    flag = struct.unpack_from("<I", blob, pos)[0]
    pos += 4
    if flag == 0x01306B47:   # fp16
        v = np.frombuffer(blob, dtype="<f2", count=n, offset=pos).astype(np.float32)
        pos += 2 * n
        pos = (pos + 3) & ~3
        return v
    assert flag == 0, "unsupported weight storage %x" % flag
    return take_floats(n)


F = TensorProto.FLOAT16 if fp16 else TensorProto.FLOAT
npf = np.float16 if fp16 else np.float32
nodes, inits = [], []


def const(name, arr):
    inits.append(numpy_helper.from_array(np.asarray(arr), name))
    return name


def shrink43(xin, name, out):
    # Bicubic 4 -> 3 shrink (scale 0.75, a = -0.75, half-pixel centres, edges repeated) as a fixed strided
    # depthwise convolution + DepthToSpace, which every GPU provider runs (DirectML has no cubic Resize).
    a = -0.75

    def cub(t):
        t = abs(t)
        if t <= 1:
            return (a + 2) * t ** 3 - (a + 3) * t ** 2 + 1
        if t < 2:
            return a * t ** 3 - 5 * a * t ** 2 + 8 * a * t - 4 * a
        return 0.0
    # output j = 3b + ph samples input x = 4b + (ph + 0.5) * 4 / 3 - 0.5; taps 4b-1 .. 4b+4 (6 inputs)
    k1 = np.zeros((3, 6), np.float64)
    for ph in range(3):
        x = (ph + 0.5) * 4 / 3 - 0.5
        f = int(np.floor(x))
        for t in range(f - 1, f + 3):
            k1[ph, t + 1] += cub(x - t)
    k1 /= k1.sum(axis=1, keepdims=True)
    wk = np.zeros((27, 1, 6, 6), np.float32)
    for c in range(3):
        for py in range(3):
            for px in range(3):
                wk[c * 9 + py * 3 + px, 0] = np.outer(k1[py], k1[px])
    nodes.extend([
        helper.make_node("Pad", [xin, const(name + "_pads", np.array([0, 0, 1, 1, 0, 0, 1, 1], np.int64))], [name + "_pad"], mode="edge"),
        helper.make_node("Conv", [name + "_pad", const(name + "_w", wk.astype(npf))], [name + "_ph"],
                         kernel_shape=[6, 6], strides=[4, 4], group=3),
        helper.make_node("DepthToSpace", [name + "_ph"], [out], blocksize=3, mode="CRD"),
    ])


# bytes -> normalised planes
nodes += [
    helper.make_node("Cast", ["in"], ["in_f"], to=TensorProto.FLOAT),
    helper.make_node("Slice", ["in_f", const("s0", np.array([0], np.int64)), const("s3", np.array([3], np.int64)),
                               const("ax3", np.array([3], np.int64))], ["in_rgb"]),
    helper.make_node("Transpose", ["in_rgb"], ["in_chw"], perm=[0, 3, 1, 2]),
    helper.make_node("Mul", ["in_chw", const("inv255", np.array(1 / 255.0, np.float32))], ["data32"]),
]
if fp16:
    nodes.append(helper.make_node("Cast", ["data32"], ["data"], to=TensorProto.FLOAT16))
else:
    nodes.append(helper.make_node("Identity", ["data32"], ["data"]))

names = {"data": "data"}
for typ, name, ins, outs, p in layers:
    if typ == "Input":
        continue
    if typ == "Split":
        for o in outs:
            names[o] = names[ins[0]]
        continue
    src = [names[i] for i in ins]
    out = outs[0]
    names[out] = out
    if typ == "Convolution":
        outc, k, n = int(p[0]), int(p[1]), int(p[6])
        inc = n // (outc * k * k)
        w = take_weights(n).reshape(outc, inc, k, k)
        b = take_floats(outc) if p.get(5, "0") == "1" else np.zeros(outc, np.float32)
        act = p.get(9, "0")
        conv_out = out if act == "0" else name + "_conv"
        nodes.append(helper.make_node("Conv", [src[0], const(name + "_w", w.astype(npf)), const(name + "_b", b.astype(npf))], [conv_out],
                                      kernel_shape=[k, k], pads=[k // 2] * 4))
        if act == "1":
            nodes.append(helper.make_node("Relu", [conv_out], [out]))
        elif act == "2":
            nodes.append(helper.make_node("LeakyRelu", [conv_out], [out], alpha=float(p[-23310][0])))
        else:
            assert act == "0", "activation %s" % act
    elif typ == "PReLU":
        c = int(p[0])
        slope = take_floats(c).reshape(c, 1, 1)
        nodes.append(helper.make_node("PRelu", [src[0], const(name + "_s", slope.astype(npf))], [out]))
    elif typ == "PixelShuffle":
        nodes.append(helper.make_node("DepthToSpace", [src[0]], [out], blocksize=int(p[0]), mode="CRD"))
    elif typ == "Interp":
        mode = {"1": "nearest", "2": "linear", "3": "cubic"}[p[0]]
        sh, sw = float(p[1]), float(p[2])
        if mode == "nearest":
            nodes.append(helper.make_node("Resize", [src[0], "", const(name + "_sc", np.array([1, 1, sh, sw], np.float32))], [out],
                                          mode="nearest", coordinate_transformation_mode="asymmetric", nearest_mode="floor"))
        else:
            assert mode == "cubic" and abs(sh - 0.75) < 1e-6 and abs(sw - 0.75) < 1e-6
            shrink43(src[0], name, out)
    elif typ == "Concat":
        assert p.get(0, "0") == "0"
        nodes.append(helper.make_node("Concat", src, [out], axis=1))
    elif typ == "Eltwise":
        assert p.get(0, "1") == "1", "only sum"
        coef = p.get(-23301, [1.0] * len(src))
        terms = []
        for k2, (x, cf) in enumerate(zip(src, coef)):
            if cf == 1.0:
                terms.append(x)
            else:
                t = "%s_t%d" % (name, k2)
                nodes.append(helper.make_node("Mul", [x, const(t + "_c", np.array(cf, npf))], [t]))
                terms.append(t)
        nodes.append(helper.make_node("Sum", terms, [out]))
    elif typ == "BinaryOp":
        assert p.get(0, "0") == "0"
        nodes.append(helper.make_node("Add", src, [out]))
    else:
        raise SystemExit("unsupported layer " + typ)
assert pos == len(blob), (pos, len(blob))

# planes -> bytes (truncating like ncnn's to_pixels), alpha 255
last = names["output"]
if add_x3:
    shrink43(last, "x3shrink", "x3out")
    last = "x3out"
if fp16:
    nodes.append(helper.make_node("Cast", [last], ["out32"], to=TensorProto.FLOAT))
    last = "out32"
nodes += [
    helper.make_node("Mul", [last, const("k255", np.array(255.0, np.float32))], ["o255"]),
    helper.make_node("Clip", ["o255", const("lo", np.array(0.0, np.float32)), const("hi", np.array(255.0, np.float32))], ["oclip"]),
    helper.make_node("Floor", ["oclip"], ["ofl"]),
    helper.make_node("Transpose", ["ofl"], ["ohwc"], perm=[0, 2, 3, 1]),
    helper.make_node("Slice", ["ohwc", const("z0", np.array([0], np.int64)), const("z1", np.array([1], np.int64)), const("zax", np.array([3], np.int64))], ["oone"]),
    helper.make_node("Mul", ["oone", const("zero", np.array(0.0, np.float32))], ["ozero"]),
    helper.make_node("Add", ["ozero", const("k255b", np.array(255.0, np.float32))], ["oalpha"]),
    helper.make_node("Concat", ["ohwc", "oalpha"], ["orgba"], axis=3),
    helper.make_node("Cast", ["orgba"], ["out"], to=TensorProto.UINT8),
]
graph = helper.make_graph(nodes, "realesr-animevideov3-x3",
                          [helper.make_tensor_value_info("in", TensorProto.UINT8, [1, "H", "W", 4])],
                          [helper.make_tensor_value_info("out", TensorProto.UINT8, [1, "OH", "OW", 4])], inits)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="scummvm-ai-upscale ncnn2onnx")
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, out_path)
print("wrote", out_path, "fp16" if fp16 else "fp32", len(nodes), "nodes")
