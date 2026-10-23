# 材质编辑与蓝图排版约定

本页说明材质图与材质实例的完整编辑链路，以及让蓝图和材质图保持整齐的排版规则。

## 材质编辑覆盖范围

材质命令分四组。图编辑直接改材质资产，实例编辑改参数覆盖，组件命令把关卡里的网格指到材质上，运行态命令验证结果。

**读取。** `get_material_graph` 返回表达式列表（含 `node_id`、类名、标题、坐标、输入和输出）、`property_inputs`（每个材质属性当前由哪个节点供数）、`comments` 和 `settings`。`list_material_parameters` 按标量、向量、纹理、静态开关分组列出参数名与默认值。`get_material_instance_parameters` 读实例的父材质与已有覆盖值。

**图编辑。** `add_material_expression` 建立节点，可在同一次调用里写 `parameter_name` 和 `properties`。`set_material_expression_property` 按反射写可编辑属性。`delete_material_expression` 删节点并把被清空的材质属性列在 `cleared_property_inputs` 里。`connect_material_expressions` 与 `disconnect_material_expressions` 管节点之间的线，引脚按名字或索引寻址。`connect_material_property` 与 `disconnect_material_property` 管材质属性上的线。`set_material_settings` 改混合模式、着色模型、材质域、双面和不透明度裁剪值。`add_material_comment` 加注释框。每次写操作后重编译材质。

**实例编辑。** `create_material_instance` 建可持久化的 `UMaterialInstanceConstant`，`set_material_instance_parameter` 写标量、向量、纹理或静态开关覆盖值。参数名必须在父材质上存在，否则命令报错而不是静默丢弃。静态开关写入时会同时打开 `bOverride`，否则值会被父材质盖掉。

**组件与运行态。** `set_material_on_component` 把关卡 Actor 上的材质槽指向材质资产。`create_dynamic_material_instance` 在 PIE 里建动态实例，`set_material_parameter` 改动态实例参数，`get_component_materials` 回读槽位与参数值。

表达式 `node_id` 优先用 `MaterialExpressionGuid`，没有有效 GUID 时回落到对象名。材质资产支持完整路径，裸资产名会依次在 `/Game/Materials` 和资产注册表里查找。

## 排版规则

蓝图和材质图使用统一的网格、间距与数据节点邻近规则。

**一条执行流占一条水平带。** 每个事件入口独占一条水平区间，流与流之间留出足够空白，任何一条流的节点都不许落进相邻流的地带。这由 `format_blueprint_graph` 的流分割实现，跨流数据线只报告为 `cross_flow_link` 警告，不会被自动断开。

**同一列共用一个 X。** 列宽取该列最宽的节点，X 由累计列宽得出，不是固定像素步长。宽节点因此不会压到右侧列。坐标对齐到 16 像素网格。

**数据节点按深度差放列。** 深度是从节点到其消费者沿数据线的最短距离。同深度的节点共用一列，读取顺序和连线方向一致。材质图里材质属性输入固定在最右列，每退一步数据深度向左移动一列。

**行内锚定消费者。** 数据节点的 Y 贴住它消费者的 Y，同一列再按顺序向下堆叠。这样读一行就能看出谁给谁供数，不需要追线。

**注释框是手工意图。** 自动排版保留注释框坐标，但目前不会检测注释框和节点重叠。排版后请在材质编辑器中检查注释框是否仍包住预期节点。注释框用来标注一组节点，不用来当容器。

**节距恒定。** 列间距 64，流间距 160，行高取节点自身高度加一个网格。恒定节距让整张图有统一的竖向节拍。

一条流内部仍然可能交叉，尤其是短链汇入长链的数据线。要再干净一些，把这些数据节点显式集中在下方一行，或加注释框标注分组，比强行重排更稳。

## 保存与撤销

材质资产写命令默认落盘，`defer_save=True` 时留在内存里，由 `save_assets` 提交。图节点的新增、删除、属性修改、连线和排版会建立编辑器事务。引擎自动化已验证表达式创建的 Undo/Redo，编辑器 UI 中 Ctrl+Z 的交互仍待验证。属性批次在事务前校验，非法输入不改变原值，也不把已保存的包标脏。保存失败发生在内存修改之后，延迟保存也不提供回滚。

## 验证

Python 契约测试在 `Python/` 下运行：

```text
.venv/Scripts/python.exe -m unittest discover -s tests
```

引擎内自动化测试覆盖真实表达式注册、参数校验、重编译、实例参数回读、无效属性拒绝和静态开关覆盖：

- `UnrealMCP.Materials.GraphEditingRoundTrip`
- `UnrealMCP.Materials.MaterialInstanceParameters`
- `UnrealMCP.Materials.PIEWorldIsolation`

命令行运行方式：

```text
UE4Editor-Cmd.exe <project>.uproject -ExecCmds="Automation RunTests UnrealMCP.Materials; Quit" -unattended -nosplash -stdout
```

端到端验证脚本通过 TCP 驱动真实编辑器，覆盖创建、连线、读图、排版、设置、实例参数与删除：

```text
.venv/Scripts/python.exe scripts/verify_material_commands.py
```

脚本需要一台已加载 UnrealMCP 的临时测试工程在 `127.0.0.1:55557` 监听。脚本在 `/Game/MCPVerifyTests` 下建立唯一命名且延迟保存的材质和实例，不调用 `save_assets`。资产不会写入磁盘，但会留在当前编辑器会话中。测试完成后关闭该临时工程以丢弃测试资产。不要在需要保留改动的日常项目编辑器中运行。

## 适用边界

材质函数资产的创建与独立编辑、Material Parameter Collection 管理、Material Layers 专用编辑和实例单项覆盖重置不在接口范围。所有节点类的反射属性、材质域渲染效果、编辑器撤销交互和大型图视觉效果，需要在隔离测试工程中分别验证。自动排版保留注释框坐标，不检测注释框避让。
