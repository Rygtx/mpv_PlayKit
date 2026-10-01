# -*- coding: utf-8 -*-
"""BT.2020 PQ 前向数学(shader 侧常量的 python 镜像)。

verify_pq_roundtrip / verify_chroma_forward 共用 —— 原两脚本各持一份逐字
拷贝,改矩阵/常数漏一处即假阴性。与 d3d12_context.cpp 的 PqToYuv/HdrToPq
shader 常量对齐(改 shader 必须同步这里)。
"""
import numpy as np

# 709 → 2020 矩阵(max RGB × 80 nit 缩放前的线性域)
M = np.array([[0.6274, 0.3293, 0.0433],
              [0.0690, 0.9195, 0.0112],
              [0.0164, 0.0880, 0.8955]], dtype=np.float64)
KR, KB = 0.2627, 0.0593
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq(nits):
    """PQ EOTF 逆(绝对亮度 nits → 归一化码值)。"""
    p = np.power(np.clip(nits / 10000.0, 0.0, 1.0), M1)
    return np.power((C1 + C2 * p) / (1.0 + C3 * p), M2)


def to_pq2020(lin709):
    """线性 709 → 线性 2020(×80 nit)→ PQ 码值。"""
    return pq(np.clip(lin709, 0, None) @ M.T * 80.0)
