"""Export an image model for the HD backgrounds (e.g. 4x-UltraSharpV2, DAT2) to ONNX for the ONNX Runtime upscalers.

The graph takes and returns RGBA bytes ([1, T, T, 4] uint8 -> [1, sT, sT, 4] uint8, alpha 255) at a fixed size T x T:
DAT lays out its attention windows with Python integers, so a traced graph is only right for the size it was traced
at (the engine runs every image as overlapping T x T tiles, see remasterOrtCreate). It computes in fp32 (UltraSharpV2
overflows to NaN in fp16); the output is rounded.

  export_onnx_fixed.py export <weights .safetensors/.pth> <out.onnx> <T>     needs torch + spandrel (no onnx package)
  export_onnx_fixed.py fp16weights <in.onnx> <out.onnx>                       needs onnx: stores the large weights as
                                                                              fp16 with a Cast back to fp32 in the graph
                                                                              (half the file; output within 1/255)
Weights are read with safetensors or torch.load(weights_only=True): no code in them runs.
"""
import sys


def export(path, out, T):
    import torch
    from spandrel import ModelLoader
    from spandrel.__helpers.canonicalize import canonicalize_state_dict

    class Wrap(torch.nn.Module):
        def __init__(self, net):
            super().__init__()
            self.net = net

        def forward(self, x):
            rgb = x[:, :, :, :3].permute(0, 3, 1, 2).float() * (1 / 255.0)
            o = self.net(rgb)
            o = torch.floor((o * 255.0 + 0.5).clamp(0, 255)).permute(0, 2, 3, 1)
            a = torch.full_like(o[:, :, :, :1], 255.0)
            return torch.cat([o, a], 3).to(torch.uint8)

    # The graph is complete before this step, which only adds onnxscript functions (none here) and needs the onnx
    # package; skipping it lets a plain torch install export.
    from torch.onnx._internal.torchscript_exporter import onnx_proto_utils
    onnx_proto_utils._add_onnxscript_fn = lambda proto, *a, **k: proto
    if path.endswith(".safetensors"):
        from safetensors.torch import load_file
        sd = load_file(path)
    else:
        sd = torch.load(path, map_location="cpu", weights_only=True)
    desc = ModelLoader().load_from_state_dict(canonicalize_state_dict(sd))
    print(desc.architecture.name, "x%d" % desc.scale)
    net = Wrap(desc.model.eval().float())
    x = torch.randint(0, 255, (1, T, T, 4), dtype=torch.uint8)
    with torch.no_grad():
        torch.onnx.export(net, (x,), out, input_names=["in"], output_names=["out"], opset_version=17,
                          do_constant_folding=True, dynamo=False)
    print("wrote", out)


def fp16weights(src, out):
    import numpy as np
    import onnx
    from onnx import TensorProto, helper, numpy_helper
    m = onnx.load(src)
    g = m.graph
    casts, keep, n = [], [], 0
    for init in g.initializer:
        a = numpy_helper.to_array(init)
        if a.dtype == np.float32 and a.size >= 1024:
            h = numpy_helper.from_array(a.astype(np.float16), init.name + "_fp16")
            keep.append(h)
            casts.append(helper.make_node("Cast", [h.name], [init.name], to=TensorProto.FLOAT, name=init.name + "_cast"))
            n += a.size
        else:
            keep.append(init)
    del g.initializer[:]
    g.initializer.extend(keep)
    nodes = list(g.node)
    del g.node[:]
    g.node.extend(casts + nodes)
    onnx.checker.check_model(m)
    onnx.save(m, out)
    print("%d weights (%d values) stored as fp16: %s" % (len(casts), n, out))


if __name__ == "__main__":
    if sys.argv[1] == "export":
        export(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif sys.argv[1] == "fp16weights":
        fp16weights(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
