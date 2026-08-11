// 测试用假客户端 (原版/Fabric 1.20.1 混淆运行时) — LivingEntity
public class bfz extends bfj {
    private cfz heldItem;

    public cfz eO() { // getMainHandItem
        return heldItem;
    }

    // 测试用: 设置手持物品
    public void bN(cfz stack) { // setHeldItem (测试辅助, 非游戏名)
        this.heldItem = stack;
    }
}
