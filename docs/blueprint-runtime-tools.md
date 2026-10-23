# 蓝图编辑与运行态验证

这些接口覆盖事件图、函数图、宏图与运行态操作，面向 Windows / UE4.26。使用前构建工程的 Editor 目标，并在自己的测试工程中验证所需行为。

## 图表

- `list_blueprint_graphs(blueprint_name)` 列出事件图、函数图、宏图及节点数。
- `create_blueprint_graph(blueprint_name, graph_name, graph_type="function")` 创建普通蓝图的函数图或宏图；`graph_type` 可为 `function`、`macro`。
- 节点查询、添加、连接、删除、排版等工具接受 `graph_name`。省略时使用第一个已有 Event Graph；查询不会创建缺失的图。
- `disconnect_blueprint_nodes` 只移除指定输出引脚到输入引脚的那一条线，保留其他连线。
- `set_blueprint_node_property` 修改反射中可编辑、非只读的节点属性，并专门支持 `NodeComment`，保护节点身份和图归属。
- `set_blueprint_pin_default` 通过图 Schema 解析并校验字符串、数值、对象/类引用及文本默认值，再设置未连接输入引脚。例如 PrintString 的 `bPrintToScreen` 是引脚默认值，不是调用节点属性。

函数图不允许事件节点和潜伏函数调用。入口/出口等引擎保护节点不能删除。图名冲突会报错。新图使用引擎生成的入口/出口；本次没有新增函数签名参数管理、宏实例节点创建或任意节点工厂。

## 编译与 C++ 反射

`compile_blueprint` 返回 `diagnostics.num_errors`、`num_warnings`、`errors`、`warnings`。失败响应在 `result.diagnostics` 保留具体消息。

`refresh_project_reflection(blueprint_name="")` 按当前已加载的反射刷新节点并编译；省略蓝图时覆盖 `/Game` 下的蓝图。刷新失败会在 `failed` 中附带具体编译诊断。刷新不保存资产。

`compile_project()` 在 `LiveCoding.Compile` 已注册时触发异步编译。`compile_triggered=true` 只表示接受了请求，不能证明编译或补丁成功；之后可用 `get_output_log` 检查输出。Live Coding 没有启用时明确报错。

新增或修改 `UFUNCTION`、`UPROPERTY` 等反射声明仍可能需要关闭编辑器、构建工程的 Editor 目标并重启。蓝图节点刷新无法生成或加载新 C++ 反射。本次没有实现外部 UBT 构建服务、任意模块卸载或自动重启编辑器。

## 保存与撤销

节点写入、图创建/排版、`compile_blueprint` 与 `set_material_on_component` 接受 `defer_save=True`。默认保存；延迟保存只让改动留在内存里。

自动排版按蓝图和图名分别记录待整理节点，并在编译前仅修改内存；最终是否保存由编译命令决定，编译失败不会因自动排版而提前落盘。

- 蓝图等资产由 `save_assets(asset_paths)` 显式保存。
- 关卡中的材质赋值由 `save_level()` 保存；未命名关卡应先用 `save_level(path="/Game/Maps/...")` 保存到合法路径。
- 图编辑在编辑器事务中记录修改，用户可通过 Ctrl+Z 撤销每次命令。
- 失败可能留下内存改动。`save_assets` 逐个保存，部分失败会返回失败状态以及 `saved` / `failed` 列表。

这些能力不等于跨调用批次、失败自动回滚或多资产原子提交。需要这些保证时，应另外设计完整的提交协议，不能把 `FScopedTransaction` 或延迟保存当作数据库事务。

## PIE 与材质验证

1. `start_pie()` 将播放请求排队，随后反复调用 `get_pie_state()`，等 `playing=true` 且 `ready=true`。命令不在游戏线程等待，避免阻塞启动。
2. `create_dynamic_material_instance(actor_name, component_name, material_index=0, pie=True)` 将 DMI 绑定到指定槽位，已有 DMI 则复用。必须存在 PIE；DMI 不持久化。
3. `set_material_parameter(..., param_name="...", scalar=0.5)` 或 `vector=[1, 0, 0]` 修改已存在的参数。两种值只能选一种；拒绝未知参数和非有限数值。
4. `get_component_materials(..., pie=True)` 直接回读网格槽位上的材质路径、是否 DMI、父材质和参数值。参数包含层关联及索引。
5. `take_screenshot(filepath="C:/tmp/material.png", pie=True)` 截取 PIE 游戏视口；`pie=False` 截取活动编辑器视口。像素截图用于人工或视觉客户端检查，本接口不自动判断图像内容是否正确。
6. `stop_pie()` 将结束请求排队，随后轮询 `get_pie_state()` 直到 `playing=false`。

所有材质工具以 Actor 名和组件名精确匹配。`pie=True` 不会在 PIE 缺失时回退到编辑器世界。目前仅寻址编辑器的主 PIE 世界，不提供多实例 PIE 选择器。

## 验证

在 `Python/` 下运行：

```text
.venv/Scripts/python.exe -m unittest discover -s tests
```

新增的 UE 自动化测试：

- `UnrealMCP.Editing.GraphScopeAndSingleLink`：函数/宏图创建、重名拒绝、延迟落盘、单线解绑、结构节点删除保护、注释/类引用/引脚默认值和只读查询。
- `UnrealMCP.Materials.PIEWorldIsolation`：PIE 缺失时不能误读编辑器 Actor。

在真实工程构建插件后，可在 Session Frontend 的 Automation 中选择上述测试。还需实测 PIE 启停、Live Coding、Ctrl+Z、保存后重载与材质参数渲染；Python 假连接测试无法证明这些引擎行为。

材质图和持久化实例接口见 [材质编辑与蓝图排版约定](material-editing.md)。
