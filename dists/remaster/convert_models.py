"""Convert the optional AI models to ncnn (.param/.bin) for the real-time upscaler (remaster_ai_net).

  convert_models.py compact <model.pth> <out base>          SRVGGNetCompact (Real-ESRGAN "Compact") state dict
  convert_models.py onnx <model.onnx> <out base> [luma]      simple conv graphs (SPAN, ArtCNN); "luma": the net
                                                             works on Y only, wrapped for RGB in the ncnn graph

The ncnn graphs take and give RGB planes in 0..1 (blobs "data" / "output"), like the standard model.
.pth files are read with torch.load(weights_only=True) (no code in them runs); .onnx files are read as plain
protobuf data by the small reader below (no onnx package needed). Weights are stored as fp16.
"""
import struct
import sys

import numpy as np

# ---------------------------------------------------------------- ONNX reading (protobuf wire format, data only)


def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def _fields(b):
    i, n = 0, len(b)
    while i < n:
        key, i = _varint(b, i)
        fn, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _varint(b, i)
        elif wt == 1:
            v = b[i:i + 8]
            i += 8
        elif wt == 2:
            ln, i = _varint(b, i)
            v = b[i:i + ln]
            i += ln
        elif wt == 5:
            v = b[i:i + 4]
            i += 4
        else:
            raise ValueError("wire type %d" % wt)
        yield fn, wt, v


_DT = {1: np.float32, 10: np.float16, 7: np.int64, 6: np.int32, 11: np.float64}


def _packed_ints(v, wt):
    if wt != 2:
        return [v if v < (1 << 63) else v - (1 << 64)]
    out, j = [], 0
    while j < len(v):
        d, j = _varint(v, j)
        out.append(d if d < (1 << 63) else d - (1 << 64))
    return out


def _tensor(b):
    dims, dt, name, raw, fl = [], 1, "", None, []
    for fn, wt, v in _fields(b):
        if fn == 1:
            dims.extend(_packed_ints(v, wt))
        elif fn == 2:
            dt = v
        elif fn == 8:
            name = v.decode()
        elif fn == 9:
            raw = v
        elif fn == 4:
            fl.extend(struct.unpack("<%df" % (len(v) // 4), v) if wt == 2 else [struct.unpack("<f", v)[0]])
    a = np.frombuffer(raw, dtype=_DT[dt]).copy() if raw is not None else np.array(fl, np.float32)
    return name, a.reshape(dims) if dims else a.reshape(())


def _attr(b):
    name, val, ints = "", None, []
    for fn, wt, v in _fields(b):
        if fn == 1:
            name = v.decode()
        elif fn == 2:
            val = struct.unpack("<f", v)[0]
        elif fn == 3:
            val = v if v < (1 << 63) else v - (1 << 64)
        elif fn == 4:
            val = v.decode(errors="replace")
        elif fn == 5:
            val = _tensor(v)[1]
        elif fn == 8:
            ints.extend(_packed_ints(v, wt))
    return name, ints if ints else val


def load_onnx(path):
    b = open(path, "rb").read()
    graph = [v for fn, wt, v in _fields(b) if fn == 7][0]
    nodes, consts, inputs, outputs = [], {}, [], []
    for fn, wt, v in _fields(graph):
        if fn == 1:
            nd = {"in": [], "out": [], "op": "", "attr": {}}
            for f2, w2, v2 in _fields(v):
                if f2 == 1:
                    nd["in"].append(v2.decode())
                elif f2 == 2:
                    nd["out"].append(v2.decode())
                elif f2 == 4:
                    nd["op"] = v2.decode()
                elif f2 == 5:
                    k, a = _attr(v2)
                    nd["attr"][k] = a
            nodes.append(nd)
        elif fn == 5:
            k, a = _tensor(v)
            consts[k] = a
        elif fn == 11:
            inputs += [v2.decode() for f2, w2, v2 in _fields(v) if f2 == 1]
        elif fn == 12:
            outputs += [v2.decode() for f2, w2, v2 in _fields(v) if f2 == 1]
    return nodes, consts, [i for i in inputs if i not in consts], outputs


# ---------------------------------------------------------------- ncnn graph building and writing


class Graph:
    def __init__(self):
        self.layers = []   # (type, name, ins, outs, params "k=v" list, weights [(array, flagged)])

    def add(self, typ, name, ins, outs, params=(), weights=()):
        self.layers.append((typ, name, list(ins), list(outs), list(params), list(weights)))

    def conv(self, name, src, dst, w, b=None, act=0):
        outc, inc, k, _ = w.shape
        p = ["0=%d" % outc, "1=%d" % k, "4=%d" % (k // 2), "5=%d" % (1 if b is not None else 0), "6=%d" % w.size]
        if act:
            p.append("9=%d" % act)
        ws = [(w.astype(np.float32), True)] + ([(b.astype(np.float32), False)] if b is not None else [])
        self.add("Convolution", name, [src], [dst], p, ws)

    def write(self, base):
        # every blob used by more than one layer goes through a Split
        uses = {}
        for typ, name, ins, outs, p, ws in self.layers:
            for i in ins:
                uses[i] = uses.get(i, 0) + 1
        layers, renamed, k = [], {}, 0
        for typ, name, ins, outs, p, ws in self.layers:
            ins2 = []
            for i in ins:
                if uses.get(i, 0) > 1:
                    lst = renamed[i]
                    ins2.append(lst.pop(0))
                else:
                    ins2.append(i)
            layers.append((typ, name, ins2, outs, p, ws))
            for o in outs:
                if uses.get(o, 0) > 1:
                    parts = ["%s_s%d" % (o, j) for j in range(uses[o])]
                    renamed[o] = list(parts)
                    layers.append(("Split", "split_%d" % k, [o], parts, [], []))
                    k += 1
        clean = lambda n: n.strip("/").replace("/", "_").replace(".", "_")   # noqa: E731
        layers = [(t, clean(nm), [clean(i) for i in i2], [clean(o) for o in o2], p, ws) for t, nm, i2, o2, p, ws in layers]
        blobs = set()
        for typ, name, ins, outs, p, ws in layers:
            blobs.update(ins)
            blobs.update(outs)
        with open(base + ".param", "w", newline="\n") as f:
            f.write("7767517\n%d %d\n" % (len(layers), len(blobs)))
            for typ, name, ins, outs, p, ws in layers:
                f.write(" ".join([typ, name, str(len(ins)), str(len(outs))] + ins + outs + p) + "\n")
        with open(base + ".bin", "wb") as f:
            for typ, name, ins, outs, p, ws in layers:
                for a, flagged in ws:
                    if flagged:   # fp16 weights: tag 0x01306B47, data padded to 4 bytes
                        h = a.astype("<f2").tobytes()
                        f.write(struct.pack("<I", 0x01306B47) + h + b"\0" * ((4 - len(h) % 4) % 4))
                    else:         # raw fp32 (biases, PReLU slopes)
                        f.write(a.astype("<f4").tobytes())
        print("wrote %s.param/.bin: %d layers, %d blobs" % (base, len(layers), len(blobs)))


def from_compact(pth, base):
    import torch
    sd = torch.load(pth, map_location="cpu", weights_only=True)
    for key in ("params_ema", "params-ema", "params", "state_dict", "model"):   # as spandrel picks it
        if isinstance(sd.get(key), dict):
            sd = sd[key]
            break
    sd = {k: v.float().numpy() for k, v in sd.items()}
    idx = sorted({int(k.split(".")[1]) for k in sd if k.startswith("body.")})
    last = idx[-1]
    g = Graph()
    g.add("Input", "data", [], ["data"])
    x = "data"
    for i in idx:
        if i % 2 == 0:   # conv
            g.conv("conv%d" % i, x, "c%d" % i, sd["body.%d.weight" % i], sd["body.%d.bias" % i])
            x = "c%d" % i
        else:            # PReLU
            s = sd["body.%d.weight" % i]
            g.add("PReLU", "prelu%d" % i, [x], ["p%d" % i], ["0=%d" % s.size], [(s, False)])
            x = "p%d" % i
    scale = int(round((sd["body.%d.weight" % last].shape[0] // 3) ** 0.5))
    g.add("PixelShuffle", "shuffle", [x], ["up"], ["0=%d" % scale])
    g.add("Interp", "base", ["data"], ["base"], ["0=1", "1=%.6e" % scale, "2=%.6e" % scale])   # nearest
    g.add("BinaryOp", "add", ["up", "base"], ["output"], ["0=0"])
    g.write(base)


def from_onnx(path, base, luma=False):
    nodes, consts, ins, outs = load_onnx(path)
    for n in nodes:
        if n["op"] == "Constant":
            consts[n["out"][0]] = np.asarray(n["attr"]["value"])
    nodes = [n for n in nodes if n["op"] != "Constant"]
    uses = {}
    for n in nodes:
        for i in n["in"]:
            uses[i] = uses.get(i, 0) + 1
    g = Graph()
    g.add("Input", "data", [], ["data"])
    names = {}
    if luma:
        # RGB -> Y (BT.709, full range) for the network; Cb, Cr resized on the side; back to RGB at the end
        kr, kg, kb = 0.2126, 0.7152, 0.0722
        g.conv("to_y", "data", "y", np.array([kr, kg, kb], np.float32).reshape(1, 3, 1, 1))
        g.conv("to_cbcr", "data", "cbcr", np.array([[-kr / 1.8556, -kg / 1.8556, (1 - kb) / 1.8556],
                                                    [(1 - kr) / 1.5748, -kg / 1.5748, -kb / 1.5748]], np.float32).reshape(2, 3, 1, 1))
        names[ins[0]] = "y"
    else:
        names[ins[0]] = "data"
    fused = set()
    for n in nodes:
        op, a = n["op"], n["attr"]
        src = [names.get(i, i) for i in n["in"]]
        o = n["out"][0]
        names[o] = o
        nm = o.strip("/").replace("/", "_") or op
        if o in fused:
            continue
        if op == "Conv":
            w = consts[n["in"][1]].astype(np.float32)
            b = consts[n["in"][2]].astype(np.float32) if len(n["in"]) > 2 else None
            k = w.shape[2]
            assert a.get("group", 1) == 1 and a.get("strides", [1, 1]) == [1, 1] and a.get("dilations", [1, 1]) == [1, 1]
            assert a.get("auto_pad", "SAME_UPPER") in ("SAME_UPPER", "NOTSET") and a.get("pads", [k // 2] * 4) == [k // 2] * 4
            # Conv followed only by Relu: fused
            nxt = [m for m in nodes if o in m["in"]]
            if len(nxt) == 1 and nxt[0]["op"] == "Relu" and uses[o] == 1:
                r = nxt[0]["out"][0]
                g.conv(nm, src[0], r, w, b, act=1)
                names[r] = r
                fused.add(r)
            else:
                g.conv(nm, src[0], o, w, b)
        elif op == "Relu":
            g.add("ReLU", nm, src[:1], [o])
        elif op == "Sigmoid":
            g.add("Sigmoid", nm, src[:1], [o])
        elif op in ("Add", "Sub", "Mul"):
            t = {"Add": 0, "Sub": 1, "Mul": 2}[op]
            c = [i for i in n["in"] if i in consts]
            if c:
                assert len(c) == 1 and consts[c[0]].size == 1 and n["in"][1] == c[0]
                g.add("BinaryOp", nm, [src[0]], [o], ["0=%d" % t, "1=1", "2=%.6e" % float(consts[c[0]])])
            else:
                g.add("BinaryOp", nm, src, [o], ["0=%d" % t])
        elif op == "Concat":
            assert a.get("axis") == 1
            g.add("Concat", nm, src, [o], ["0=0"])
        elif op == "DepthToSpace":
            g.add("PixelShuffle", nm, src[:1], [o], ["0=%d" % a["blocksize"], "1=%d" % (1 if a.get("mode", "DCR") == "DCR" else 0)])
        elif op == "Clip":
            lo = float(consts[n["in"][1]]) if len(n["in"]) > 1 and n["in"][1] else -3.4e38
            hi = float(consts[n["in"][2]]) if len(n["in"]) > 2 and n["in"][2] else 3.4e38
            g.add("Clip", nm, src[:1], [o], ["0=%.6e" % lo, "1=%.6e" % hi])
        else:
            raise SystemExit("unsupported ONNX op " + op)
    last = names[outs[0]]
    if luma:
        # the network's own output blob becomes "y2" (the graph's "output" comes after the colour conversion)
        g.layers = [(t, nm, [("y2" if i == last else i) for i in i2], [("y2" if o == last else o) for o in o2], p, ws)
                    for t, nm, i2, o2, p, ws in g.layers]
        last = "y2"
        g.add("Interp", "cbcr_up", ["cbcr"], ["cbcr2"], ["0=3", "1=2.000000e+00", "2=2.000000e+00"])   # bicubic
        g.add("Concat", "ycc", [last, "cbcr2"], ["ycc"], ["0=0"])
        kr, kg, kb = 0.2126, 0.7152, 0.0722
        m = np.array([[1, 0, 1.5748], [1, -kb * 1.8556 / kg, -kr * 1.5748 / kg], [1, 1.8556, 0]], np.float32)
        g.conv("to_rgb", "ycc", "output", m.reshape(3, 3, 1, 1))
    else:
        # rename the last blob to "output"
        typ, name, i2, o2, p, ws = g.layers[-1]
        g.layers[-1] = (typ, name, i2, ["output"], p, ws)
    g.write(base)


if __name__ == "__main__":
    if sys.argv[1] == "compact":
        from_compact(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "onnx":
        from_onnx(sys.argv[2], sys.argv[3], "luma" in sys.argv[4:])
    else:
        raise SystemExit(__doc__)
