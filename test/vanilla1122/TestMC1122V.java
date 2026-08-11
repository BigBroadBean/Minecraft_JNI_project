// 端到端测试: 模拟原版 1.12.2 (混淆运行时) 的 "Minecraft 1.12.2" 窗口
// 判定预期:
//   canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
//   canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
import javax.swing.JFrame;

public class TestMC1122V {
    public static void main(String[] args) throws Exception {
        bib mc = bib.z();
        bud player = new bud();
        mc.h = player;

        JFrame frame = new JFrame("Minecraft 1.12.2");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        // 物品: 放置物 (ahb=ItemBlock) 与 普通物品 (ain=Item)
        aip dirtStack = new aip(new ahb());
        aip swordStack = new aip(new ain());

        System.out.println("test(vanilla1122): 阶段1 未瞄准+空手 (canAttack=0 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.s = new bhc(new vp()); // 目标要是 EntityLivingBase (vp), 不能是 Entity (vg)
        System.out.println("test(vanilla1122): 阶段2 瞄准生物+手持ItemBlock (canAttack=1 canPlace=1) 4 秒...");
        Thread.sleep(4000);

        player.setHeldItem(swordStack);
        System.out.println("test(vanilla1122): 阶段3 瞄准生物+手持普通Item (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(null);
        System.out.println("test(vanilla1122): 阶段4 瞄准生物+空手 (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.s = null;
        System.out.println("test(vanilla1122): 阶段5 未瞄准+手持ItemBlock (canAttack=0 canPlace=1) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(vanilla1122): 结束");
        System.exit(0);
    }
}
