// 测试用假客户端 (原版 1.8.9 混淆运行时)
// 注意: 真实 1.8.9 里 auh$a 是 auh 的内部枚举, 这里用顶层类模拟
// (JVM 类名同样是 auh$a, 对 JNI FindClass 无区别)
public enum auh$a {
    a, b, c // MISS, BLOCK, ENTITY
}
