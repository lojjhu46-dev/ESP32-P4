F8 模拟器 UI 升级（同源 ui_shared.c，纯 LVGL；固件小改一处）

## A. 固件小改（jc_p4_app/main/app_faceai.cpp）

`send_result` 的 lib 数组旁新增**平行数组 `lib_feats`**（每个唯一名字的特征条数，从 s_names 按名计数），`list` 命令回包同样携带。向后兼容（旧 UI/脚本忽略新字段）。这是"F8-名单显示每人特征数"的数据源。

## B. 模拟器 UI（lvgl_project/lv_port_pc_vscode_official/main/src/ui_shared.c）

1. **Register 弹窗升级**（open_input :332 + input_ok_cb :312）：
   - 弹窗加 shots 选择行（1/3/5 三个小按钮，单选高亮，默认 3）；
   - OK 后弹窗转为"注册中"状态：隐藏键盘/输入框，显示进度条 + "collected N/3"（set_result :990 拦截 event=="enrolling"，sscanf msg 驱动）；
   - registered/error 事件自动关弹窗；采集期间 Cancel 发 `{"cmd":"stop"}`（板端语义=取消采集）。
2. **结果行稳定态**（set_result :880 who 计算）：`stable==1&&match` → "OK 名字 sim"；tracking → ".. 确认中"；stable reject → "?? 陌生人"；其余保持现格式。用 ASCII（LVGL 内置字体无 ✓）。
3. **检测反馈行（F8 硬需求）**：结果行加 `w323px`（faces>0 时取 boxes[0] 宽度）——实时"是否检到脸+脸宽"。
4. **每框名字/相似度标签**（F7 配套收尾）：json_boxes（:130）放宽解析第 5/6 元素（name/sim）→ 新数组 box_name[]；update_boxes（:281）从"只画第一框"放开为"有名字就画"，标签文字 = 名字+sim。
5. **名单特征数**：json_lib（:161）扩展解析 `lib_feats`；lbl_list 显示 "lib: me(3) bill(2)"。

## C. 验证（板子在线，hub 当前已停，模拟器直连 192.168.33.2:3333）

1. WSL 编译模拟器（cd build && cmake .. && make -j）；
2. 固件先编译烧录（lib_feats 改动，~6 分钟）；
3. 真人走查（你在镜头前）：三帧注册（弹窗进度 1/3→3/3）、识别稳定态（OK me）、检测反馈（w=px 实时）、名单 me(3)；
4. 回归：_face_r1_test.py --tcp 经中枢跑一遍确认固件 lib_feats 改动无回归；模拟器改动不涉固件。

## D. 文档

HANDOVER 第廿二节追加 16 节（F8 实现+验证记录）；face_recog_day_plan.md F8 标记完成；记忆更新。