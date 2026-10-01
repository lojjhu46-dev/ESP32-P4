# -*- coding: utf-8 -*-
"""给本地检出的 esp_cam_sensor（D:\\esp32project\\ESP32-P4\\esp_cam_sensor_local）
的 sc2336 驱动打 patch：增加"帧率/VTS"控制，供夜视长曝光使用。

做四件事：
 1) struct sc2336_cam 增加 RAM 副本（格式表在 flash 里是 const，改 VTS 必须换副本）
 2) 新增 sc2336_set_fps()：按 base_vts*base_fps/fps 算新 VTS 写 0x320E/0x320F，
    并同步曝光上限（exp_max = VTS-6）
 3) sc2336_set_format() 里记录 base_vts/base_fps
 4) set/query para 里加 ESP_CAM_SENSOR_FPS 分支

用法: python tools/patch_sc2336.py
（幂等：已打过就跳过）
"""
import io
import os

PATH = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", "..", "esp_cam_sensor_local", "sensors", "sc2336", "sc2336.c"))

with io.open(PATH, encoding="utf-8") as f:
    src = f.read()

if "sc2336_set_fps" in src:
    print("SKIP: 已经打过 patch")
    raise SystemExit(0)

changes = []

# ---- 1) struct ----
old1 = """struct sc2336_cam {
    sc2336_para_t sc2336_para;
};"""
new1 = """struct sc2336_cam {
    sc2336_para_t sc2336_para;

    /* ---- 本地 patch：帧率/VTS 控制用 ----
     * 格式表（含 isp_v1_info.vts）在 flash 里是 const，不能直接改；改 VTS 时把当前
     * 格式复制到这两块 RAM 副本里、让 dev->cur_format 指向副本，
     * 这样曝光上限（= vts - 6，见 query_para_desc）会跟着变。 */
    esp_cam_sensor_format_t   fmt;
    esp_cam_sensor_isp_info_t isp_info;
    uint32_t                  base_vts;   /* 格式表原始 VTS */
    uint32_t                  base_fps;   /* 格式表标称帧率 */
};"""
changes.append(("struct", old1, new1))

# ---- 2) set_fps（插在 set_exp_val 之后） ----
old2 = """    if (ret == ESP_OK) {
        cam_sc2336->sc2336_para.exposure_val = value_buf;
    }
    return ret;
}

static esp_err_t sc2336_set_total_gain_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)"""
new2 = """    if (ret == ESP_OK) {
        cam_sc2336->sc2336_para.exposure_val = value_buf;
    }
    return ret;
}

/**
 * @brief 本地 patch：设置帧率（内部改 VTS 帧长）
 *
 * 帧率越低，一帧的行数（VTS）越多，单帧可用的曝光行数越多（上限 = VTS-6），
 * 也就是夜视能拿到的曝光时间越长。基准 vts/fps 取自格式表（set_format 时记录）。
 */
static esp_err_t sc2336_set_fps(esp_cam_sensor_device_t *dev, uint32_t fps)
{
    esp_err_t ret;
    struct sc2336_cam *cam = (struct sc2336_cam *)dev->priv;
    uint32_t base_fps = cam->base_fps ? cam->base_fps : 50;
    uint32_t base_vts = cam->base_vts ? cam->base_vts
                                      : dev->cur_format->isp_info->isp_v1_info.vts;
    uint32_t min_vts = (uint32_t)dev->cur_format->height + 8;

    if (fps == 0) {
        fps = 1;
    }
    if (fps > base_fps) {
        fps = base_fps;                     /* 比格式标称更快没意义 */
    }
    uint32_t vts = (uint32_t)(((uint64_t)base_vts * base_fps + fps / 2) / fps);
    if (vts < min_vts) {
        vts = min_vts;
    }
    if (vts > SC2336_VTS_MAX) {
        vts = SC2336_VTS_MAX;
    }

    ret = sc2336_write(dev->sccb_handle, 0x320e, (vts >> 8) & 0xff);
    if (ret == ESP_OK) {
        ret = sc2336_write(dev->sccb_handle, 0x320f, vts & 0xff);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set vts %" PRIu32 " failed", vts);
        return ret;
    }

    cam->isp_info = *dev->cur_format->isp_info;
    cam->isp_info.isp_v1_info.vts = vts;
    cam->fmt = *dev->cur_format;
    cam->fmt.isp_info = &cam->isp_info;
    dev->cur_format = &cam->fmt;

    cam->sc2336_para.exposure_max = vts - SC2336_EXP_MAX_OFFSET;
    if (cam->sc2336_para.exposure_val > cam->sc2336_para.exposure_max) {
        sc2336_set_exp_val(dev, cam->sc2336_para.exposure_max);
    }
    ESP_LOGW(TAG, "fps -> %" PRIu32 " (vts %" PRIu32 ", exp_max %" PRIu32 ")",
             fps, vts, cam->sc2336_para.exposure_max);
    return ESP_OK;
}

static esp_err_t sc2336_set_total_gain_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)"""
changes.append(("set_fps", old2, new2))

# ---- 3) set_format 里记录基准 ----
old3 = """    cam_sc2336->sc2336_para.exposure_max = dev->cur_format->isp_info->isp_v1_info.vts - SC2336_EXP_MAX_OFFSET;

    return ret;
}

static esp_err_t sc2336_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)"""
new3 = """    cam_sc2336->sc2336_para.exposure_max = dev->cur_format->isp_info->isp_v1_info.vts - SC2336_EXP_MAX_OFFSET;
    /* 本地 patch：记下格式表的基准 vts/fps，供 sc2336_set_fps() 换算（避免累积误差）*/
    cam_sc2336->base_vts = dev->cur_format->isp_info->isp_v1_info.vts;
    cam_sc2336->base_fps = dev->cur_format->fps ? dev->cur_format->fps : 50;

    return ret;
}

static esp_err_t sc2336_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)"""
changes.append(("set_format", old3, new3))

# ---- 4) set_para_value 加 case ----
old4 = """        ret |= sc2336_write(dev->sccb_handle, SC2336_REG_GROUP_HOLD, SC2336_GROUP_HOLD_END);
        break;
    }"""
new4 = """        ret |= sc2336_write(dev->sccb_handle, SC2336_REG_GROUP_HOLD, SC2336_GROUP_HOLD_END);
        break;
    }
    case ESP_CAM_SENSOR_FPS: {
        /* 本地 patch：帧率 -> VTS（夜视长曝光）*/
        uint32_t u32_val = *(uint32_t *)arg;
        ret = sc2336_set_fps(dev, u32_val);
        break;
    }"""
changes.append(("set_para", old4, new4))

# ---- 5) query_para_desc 加 case ----
old5 = """    case ESP_CAM_SENSOR_EXPOSURE_US:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;"""
new5 = """    case ESP_CAM_SENSOR_FPS: {
        /* 本地 patch：帧率范围（1 ~ 格式标称帧率）*/
        struct sc2336_cam *cam = (struct sc2336_cam *)dev->priv;
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 1;
        qdesc->number.maximum = cam->base_fps ? cam->base_fps : 50;
        qdesc->number.step = 1;
        qdesc->default_value = cam->base_fps ? cam->base_fps : 50;
        break;
    }
    case ESP_CAM_SENSOR_EXPOSURE_US:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;"""
changes.append(("query_para", old5, new5))

for name, old, new in changes:
    if old not in src:
        print("FAIL: 锚点找不到 -> " + name)
        raise SystemExit(1)
    if src.count(old) != 1:
        print("FAIL: 锚点不唯一(%d) -> %s" % (src.count(old), name))
        raise SystemExit(1)
    src = src.replace(old, new, 1)
    print("ok: " + name)

with io.open(PATH, "w", encoding="utf-8", newline="") as f:
    f.write(src)
print("done: sc2336 patched")
