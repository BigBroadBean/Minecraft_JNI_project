// 测试用假客户端 (原版 1.12.2 混淆运行时) — EntityLivingBase (vp)
// 1.9+ 双持: co = getHeldItemMainhand (无参 getHeldItem 已移除)
public class vp extends vg {
    private aip heldItemMainhand;

    public aip co() { // getHeldItemMainhand
        return heldItemMainhand;
    }

    // 测试用: 设置手持物品 (非游戏名)
    public void setHeldItem(aip stack) {
        this.heldItemMainhand = stack;
    }
}
