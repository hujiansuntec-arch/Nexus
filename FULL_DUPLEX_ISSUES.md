# 全双工测试遇到的问题分析

## 问题1: Double Free崩溃 ❌

### 现象
```
double free or corruption (out)
Aborted (core dumped)
```

### 根本原因
在`runWorker`函数中，`node`是局部变量（`auto node = createNode(node_id)`），当函数返回时：
1. node的unique_ptr被销毁
2. NodeImpl的析构函数被调用
3. 但全局atomic变量（g_sent, g_received等）在callback中仍在被访问
4. **callback可能在node析构后仍被触发**，导致访问已释放的内存

### 触发条件
- 高速发送场景（100万+ msg/s）
- 主函数退出但接收线程还在运行
- 析构顺序问题

## 问题2: 消息丢失严重 ⚠️

### 数据
```
node0: 发送14,915,009 消息，接收649,167 (4.35%)
node1: 发送9,434,421 消息，接收602,525 (6.39%)
```

### 原因分析
1. **发送速度过快**: 124万msg/s远超队列容量
2. **队列溢出**: 
   - V3每个队列容量1024条消息
   - 高速发送直接填满队列
3. **无流控**: 没有背压机制，发送方不知道接收方负载
4. **CPU竞争**: 发送和接收在同一进程，CPU调度不均

### 计算
```
发送速率: 1,242,917 msg/s
队列容量: 1024 条
填满时间: 1024 / 1,242,917 ≈ 0.82ms
```
队列不到1毫秒就被填满！

## 问题3: 回显逻辑缺陷 🔍

### 代码片段
```cpp
// 向每个伙伴发送
for (int i = 0; i < partner_count; i++) {
    std::string target = "to_node" + std::to_string(i);
    if (target != my_topic) {  // 不发给自己
        node->broadcast("duplex", target, ...);
    }
}
```

### 问题
- **partner_count=2时**，node0只向`to_node1`发送
- 但node1也向`to_node0`和`to_node1`发送
- **逻辑不对称**：应该只发给对方，而不是所有可能的节点

## 问题4: 性能瓶颈 📊

### 观察
```
发送速率: 1,242,917 msg/s  (非常快!)
接收速率:    54,097 msg/s  (慢23倍!)
```

### 分析
1. **发送是单向写入**，只需一次内存拷贝
2. **接收需要**：
   - 轮询队列
   - 反序列化消息
   - 调用callback
   - 再次发送（回显）
3. **CPU时间分配不均**

## 建议修复方案

### 方案1: 添加流控
```cpp
// 每1000条休息更长时间
if (g_sent % 1000 == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 改为1ms
}
```

### 方案2: 降低发送速度
```cpp
// 每条消息后都休息
if (g_sent % 100 == 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}
```

### 方案3: 修复节点定位逻辑
```cpp
// 只向真实存在的对方节点发送
std::string partner_id = "node" + std::to_string(1 - std::stoi(node_id.substr(4)));
std::string target = "to_" + partner_id;
node->broadcast("duplex", target, payload);
```

### 方案4: 分离发送和接收进程
- 一个进程只发送
- 另一个进程只接收+回显
- 避免CPU竞争

### 方案5: 修复析构顺序
```cpp
// 在main中使用shared_ptr保持生命周期
auto node = createNode(node_id);
// ... 使用node
node.reset();  // 显式释放
std::this_thread::sleep_for(std::chrono::seconds(1));  // 等待callback完成
```

## 实际测试结果总结

### 成功验证 ✅
1. **V3可以工作**: 建立连接、发送接收都正常
2. **高吞吐量**: 单向发送达到120万msg/s
3. **全双工可行**: 同时收发确实工作

### 需要改进 ⚠️
1. 流控机制
2. 队列容量调优
3. 生命周期管理
4. 更合理的测试场景（降低速度）

## 推荐的改进测试

### 低速稳定测试
```bash
# 每秒10,000条消息，持续60秒
./test_duplex node0 2 60 256 --rate 10000
./test_duplex node1 2 60 256 --rate 10000
```

### 中速压力测试
```bash
# 每秒100,000条消息，持续10秒
./test_duplex node0 2 10 256 --rate 100000
./test_duplex node1 2 10 256 --rate 100000
```

这样可以更好地测试稳定性和实际应用场景。
