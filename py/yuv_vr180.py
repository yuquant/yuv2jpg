#!/usr/bin/env python3
"""
配对处理 frontFishEye / backFishEye YUV420 文件，生成 VR180 SBS JPG。

文件名示例:
    20261021155533_backFishEye.yuv
    20261021155533_frontFishEye.yuv

流程:
  1. 按时间戳前缀配对 front / back
  2. 右眼 (默认 back) 顺时针旋转 0.6° 矫正
  3. 鱼眼 (equidistant) 解算为 VR180 半幅等距柱状投影
  4. 左右眼水平拼接成 SBS，垂直翻转（修正上下颠倒），保存 JPG
  5. 缓存 remap LUT（按分辨率+FOV），下次直接加载加速
  6. 若检测到 CUDA 设备则尝试 GPU remap 加速（无则自动回退 CPU）

用法:
    python3 yuv_vr180.py                 # 当前目录所有配对
    python3 yuv_vr180.py a_front...yuv b_back...yuv
    python3 yuv_vr180.py --fmt i420
    python3 yuv_vr180.py --fov 208 --right-rot 0.6 --left front
"""

import os
import sys
import glob
import math
import re
from collections import defaultdict
import numpy as np
import cv2
from PIL import Image

# ---- 常见分辨率候选（宽, 高）----
COMMON_SIZES = [
    (3840, 2160), (1920, 1080), (1280, 720), (1024, 768),
    (1280, 960),  (640, 480),   (800, 600),  (2560, 1440),
    (3840, 3840), (1024, 1024), (1280, 1280), (2048, 2048),
    (2160, 2160), (2880, 2880), (4096, 4096),
]


def guess_wh(file_size, verbose=True):
    """根据 YUV420 文件大小反推分辨率。单帧 = W*H*3/2"""
    wh = file_size * 2 // 3
    side = int(math.isqrt(wh))
    if side * side == wh and side % 2 == 0:
        frame = side * side * 3 // 2
        if frame > 0 and file_size % frame == 0:
            return side, side, file_size // frame

    for w, h in COMMON_SIZES:
        frame = w * h * 3 // 2
        if frame > 0 and file_size % frame == 0:
            return w, h, file_size // frame

    if file_size < 50 * 1024 * 1024:
        target = file_size * 2 // 3
        for w in range(2, int(math.isqrt(target)) * 2, 2):
            if target % w == 0:
                h = target // w
                if h % 2 == 0 and w * h == target:
                    return w, h, 1

    return None, None, None


def load_yuv_first_frame(path, fmt="nv12"):
    """读取 YUV 文件并返回第一帧 BGR 图像 + (W,H,n_frames)"""
    data = np.fromfile(path, dtype=np.uint8)
    size = data.size
    W, H, n_frames = guess_wh(size)
    if W is None:
        raise ValueError(f"无法推断分辨率: {path} ({size} bytes)")

    frame_size = W * H * 3 // 2
    frame = data[:frame_size]
    y_size = W * H
    uv_size = W * H // 2

    y_plane = frame[:y_size].reshape((H, W))

    if fmt == "i420":
        uv_h = H // 2
        uv_w = W // 2
        u_plane = frame[y_size:y_size + uv_h * uv_w].reshape((uv_h, uv_w))
        v_plane = frame[y_size + uv_h * uv_w: y_size + 2 * uv_h * uv_w].reshape((uv_h, uv_w))
        i420 = np.vstack([y_plane, u_plane, v_plane])
        bgr = cv2.cvtColor(i420, cv2.COLOR_YUV2BGR_I420)
    else:  # nv12
        uv_plane = frame[y_size:y_size + uv_size].reshape((H // 2, W))
        nv12 = np.vstack([y_plane, uv_plane])
        bgr = cv2.cvtColor(nv12, cv2.COLOR_YUV2BGR_NV12)

    return bgr, W, H, n_frames


def rotate_image(img, angle_deg, clockwise=True):
    """绕图像中心旋转。clockwise=True 表示顺时针。"""
    if abs(angle_deg) < 1e-6:
        return img
    h, w = img.shape[:2]
    center = (w / 2.0, h / 2.0)
    # OpenCV 正角度为逆时针
    ang = -angle_deg if clockwise else angle_deg
    M = cv2.getRotationMatrix2D(center, ang, 1.0)
    return cv2.warpAffine(img, M, (w, h), flags=cv2.INTER_LINEAR,
                          borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0))


def create_fisheye_to_equirect_map(src_w, src_h, out_w, out_h,
                                   fov_deg=208.0, cx=None, cy=None, radius=None):
    """
    生成 equidistant 鱼眼 -> VR180 等距柱状投影 (经度 ±90°, 纬度 ±90°) 的 remap 表。
    假设光学中心在图像中心，圆形有效区域半径 = min(w,h)/2。
    """
    if cx is None:
        cx = src_w / 2.0
    if cy is None:
        cy = src_h / 2.0
    if radius is None:
        radius = min(src_w, src_h) / 2.0

    fov = np.deg2rad(fov_deg)

    xs = np.linspace(0, out_w - 1, out_w, dtype=np.float32)
    ys = np.linspace(0, out_h - 1, out_h, dtype=np.float32)
    xv, yv = np.meshgrid(xs, ys)

    # 经度 λ ∈ [-π/2, π/2], 纬度 φ ∈ [π/2, -π/2]
    lon = (xv / max(out_w - 1, 1) - 0.5) * np.pi
    lat = (0.5 - yv / max(out_h - 1, 1)) * np.pi

    cos_lat = np.cos(lat)
    X = cos_lat * np.sin(lon)
    Y = np.sin(lat)
    Z = cos_lat * np.cos(lon)

    # 与光轴夹角 θ
    theta = np.arccos(np.clip(Z, -1.0, 1.0))

    # equidistant: r ∝ θ
    r = (theta / (fov / 2.0)) * radius

    phi = np.arctan2(Y, X)
    map_x = (cx + r * np.cos(phi)).astype(np.float32)
    map_y = (cy + r * np.sin(phi)).astype(np.float32)

    return map_x, map_y


def get_or_create_lut(src_w, src_h, out_w, out_h, fov_deg=208.0, lut_dir="."):
    """
    加载或生成 remap LUT，并缓存到磁盘（.npz）。
    文件名包含分辨率与 FOV，便于复用。
    """
    os.makedirs(lut_dir, exist_ok=True)
    lut_name = f"lut_{src_w}x{src_h}_to_{out_w}x{out_h}_fov{fov_deg:.1f}.npz"
    lut_path = os.path.join(lut_dir, lut_name)

    if os.path.isfile(lut_path):
        try:
            data = np.load(lut_path)
            map_x = data["map_x"]
            map_y = data["map_y"]
            if map_x.shape == (out_h, out_w) and map_y.shape == (out_h, out_w):
                print(f"  加载缓存 LUT: {lut_name}")
                return map_x, map_y
        except Exception as e:
            print(f"  [警告] LUT 加载失败 ({e})，重新生成")

    print(f"  生成新 LUT: {lut_name}")
    map_x, map_y = create_fisheye_to_equirect_map(
        src_w, src_h, out_w, out_h, fov_deg=fov_deg
    )
    np.savez_compressed(lut_path, map_x=map_x, map_y=map_y)
    print(f"  LUT 已保存: {lut_path}")
    return map_x, map_y


def try_cuda_remap(img, map_x, map_y):
    """若存在可用 CUDA 设备则使用 GPU remap，否则回退 CPU。"""
    try:
        if hasattr(cv2, "cuda") and cv2.cuda.getCudaEnabledDeviceCount() > 0:
            gpu_img = cv2.cuda_GpuMat()
            gpu_img.upload(img)
            gpu_map_x = cv2.cuda_GpuMat()
            gpu_map_y = cv2.cuda_GpuMat()
            gpu_map_x.upload(map_x)
            gpu_map_y.upload(map_y)
            gpu_out = cv2.cuda.remap(gpu_img, gpu_map_x, gpu_map_y,
                                     interpolation=cv2.INTER_LINEAR,
                                     borderMode=cv2.BORDER_CONSTANT)
            return gpu_out.download()
    except Exception:
        pass
    return cv2.remap(img, map_x, map_y, cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0))


def process_pair(front_path, back_path, fmt="nv12", fov=208.0,
                 right_rot_deg=0.6, left_is="front", outdir=None):
    """
    处理一对 front/back，输出 SBS VR180 JPG。
    left_is: 'front' 或 'back'，决定哪只眼睛是左眼。
    """
    print(f"\n=== 配对 ===")
    print(f"  front: {os.path.basename(front_path)}")
    print(f"  back : {os.path.basename(back_path)}")

    front_bgr, Wf, Hf, nf = load_yuv_first_frame(front_path, fmt)
    back_bgr, Wb, Hb, nb = load_yuv_first_frame(back_path, fmt)

    if (Wf, Hf) != (Wb, Hb):
        print(f"  [警告] 分辨率不一致 front={Wf}x{Hf} back={Wb}x{Hb}，将以 front 为准")
    W, H = Wf, Hf
    print(f"  分辨率: {W}x{H}, front帧数={nf}, back帧数={nb} (仅取第1帧)")

    # 分配左右眼
    if left_is.lower() == "front":
        left_img = front_bgr
        right_img = back_bgr
        left_name, right_name = "front", "back"
    else:
        left_img = back_bgr
        right_img = front_bgr
        left_name, right_name = "back", "front"

    print(f"  左眼={left_name}, 右眼={right_name}")

    # 右眼顺时针旋转矫正
    if abs(right_rot_deg) > 1e-6:
        print(f"  右眼顺时针旋转 {right_rot_deg}°")
        right_img = rotate_image(right_img, right_rot_deg, clockwise=True)

    # 输出尺寸：每眼正方形，SBS 为 2:1
    out_h = H
    out_w = H  # 单眼 180° 正方形
    print(f"  鱼眼→VR180 (FOV={fov}°), 单眼输出 {out_w}x{out_h}")

    # LUT 缓存目录（与输出同目录，或当前目录）
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        lut_dir = outdir
    else:
        lut_dir = os.path.dirname(front_path) or "."

    map_x, map_y = get_or_create_lut(W, H, out_w, out_h, fov_deg=fov, lut_dir=lut_dir)

    # remap (尝试 GPU)
    left_eq = try_cuda_remap(left_img, map_x, map_y)
    right_eq = try_cuda_remap(right_img, map_x, map_y)

    # SBS + 垂直翻转（修正上下颠倒）
    sbs = np.hstack([left_eq, right_eq])
    sbs = cv2.flip(sbs, 0)
    print(f"  SBS 尺寸: {sbs.shape[1]}x{sbs.shape[0]} (已垂直翻转)")

    # 输出路径
    base = os.path.basename(front_path)
    # 提取时间戳前缀
    m = re.match(r"^(\d+)_", base)
    prefix = m.group(1) if m else os.path.splitext(base)[0]

    out_path = os.path.join(lut_dir, f"{prefix}_VR180_SBS.jpg")
    # OpenCV 是 BGR，Pillow 要 RGB；subsampling=0 即 4:4:4
    sbs_rgb = cv2.cvtColor(sbs, cv2.COLOR_BGR2RGB)
    Image.fromarray(sbs_rgb).save(
        out_path,
        format="JPEG",
        quality=95,
        subsampling=0,
        optimize=True,
    )
    print(f"  -> {out_path}")
    return out_path
    print(f"  -> {out_path}")
    return out_path


def find_pairs(files):
    """
    从文件列表中找出 front/back 配对。
    支持命名: *frontFishEye.yuv / *backFishEye.yuv 或 *front*.yuv / *back*.yuv
    """
    groups = defaultdict(dict)
    for f in files:
        base = os.path.basename(f).lower()
        # 提取前缀（时间戳或去掉 front/back 的部分）
        if "front" in base:
            key = re.sub(r"[_\-]?front.*?fisheye|\.yuv$", "", base, flags=re.I)
            groups[key]["front"] = f
        elif "back" in base or "bac" in base:
            key = re.sub(r"[_\-]?bac?k.*?fisheye|\.yuv$", "", base, flags=re.I)
            groups[key]["back"] = f
        else:
            # 无法识别，跳过
            continue

    pairs = []
    for key, d in sorted(groups.items()):
        if "front" in d and "back" in d:
            pairs.append((d["front"], d["back"]))
        else:
            missing = "back" if "front" in d else "front"
            print(f"[跳过] 前缀 '{key}' 缺少 {missing}")
    return pairs


def main():
    argv = sys.argv[1:]
    fmt = "nv12"
    fov = 208.0
    right_rot = 0.6
    left_is = "front"
    outdir = None

    # 简单参数解析
    i = 0
    files = []
    while i < len(argv):
        a = argv[i]
        if a == "--fmt" and i + 1 < len(argv):
            fmt = argv[i + 1].lower()
            i += 2
        elif a == "--fov" and i + 1 < len(argv):
            fov = float(argv[i + 1])
            i += 2
        elif a == "--right-rot" and i + 1 < len(argv):
            right_rot = float(argv[i + 1])
            i += 2
        elif a == "--left" and i + 1 < len(argv):
            left_is = argv[i + 1].lower()
            i += 2
        elif a == "--outdir" and i + 1 < len(argv):
            outdir = argv[i + 1]
            i += 2
        elif a.startswith("-"):
            print(f"未知参数: {a}")
            i += 1
        else:
            files.append(a)
            i += 1

    if not files:
        files = sorted(glob.glob("*.yuv"))
        if not files:
            print("当前目录没有 *.yuv 文件")
            return

    print(f"共扫描 {len(files)} 个文件, 格式={fmt}, FOV={fov}°, 右眼旋转={right_rot}°, 左眼={left_is}")

    pairs = find_pairs(files)
    if not pairs:
        print("未找到任何 front/back 配对。请确认文件名包含 frontFishEye / backFishEye")
        return

    print(f"找到 {len(pairs)} 对")

    use_cuda = hasattr(cv2, "cuda") and cv2.cuda.getCudaEnabledDeviceCount() > 0
    print(f"CUDA 加速: {'可用' if use_cuda else '不可用 (回退 CPU)'}")

    for front, back in pairs:
        try:
            process_pair(front, back, fmt=fmt, fov=fov,
                         right_rot_deg=right_rot, left_is=left_is, outdir=outdir)
        except Exception as e:
            print(f"  [错误] {front} + {back}: {e}")
            import traceback
            traceback.print_exc()


if __name__ == "__main__":
    main()
