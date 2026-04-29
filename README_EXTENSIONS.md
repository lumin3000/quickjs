# QuickJS 扩展模块

这个目录包含了为 QuickJS 开发的通用扩展，补充 QuickJS 缺失的功能。

## 扩展列表

### 1. quickjs_searchpath
**功能**：提供 Lua 风格的文件查找功能
- 完全对齐 Lua 的 package.searchpath
- 支持路径模板（`?` 占位符）
- 支持多路径搜索（`;` 分隔）
- 点号自动转换为路径分隔符

**文件**：
- `quickjs_searchpath.c` - 实现（基于 Lua loadlib.c）
- `quickjs_searchpath.h` - 接口

**用途**：动态查找和加载模块文件

### 2. quickjs_stackful_mini
**功能**：基于 Tina 的 stackful 协程层
- 类似 Lua 的 coroutine.yield/resume，但不需要业务代码写 generator
- C 协程切栈通过 Tina 提供
- 数据存储和调度器内置

**文件**：
- `quickjs_stackful_mini.c` - 实现
- `quickjs_stackful_mini.h` - 接口
- `tina.h` - Tina 协程库

**用途**：jtask service 的协程切换

## 历史

之前还有一个 `quickjs_coroutine.c` 扩展（Generator-based 协程调度），
在 2026-04-29 移除——它被注册但 0 处实际调用，由 `quickjs_stackful_mini`
取代。

## 设计原则

1. **通用性** - 不依赖特定项目
2. **对齐 Lua** - 行为和接口尽量与 Lua 保持一致
3. **独立性** - 每个扩展独立
4. **高性能** - C 实现，最小化开销
