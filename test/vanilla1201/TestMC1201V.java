// 端到端测试: 模拟原版/Fabric 1.20.1 (混淆运行时) 的 "Minecraft* 1.20.1" 窗口
// 判定预期:
//   canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
//   canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
import javax.swing.JFrame;

public class TestMC1201V {
    public static void main(String[] args) throws Exception {
        enn mc = enn.N();
        fiy player = new fiy();
        mc.t = player;

        JFrame frame = new JFrame("Minecraft* 1.20.1");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        // 物品: 放置物 (cds=BlockItem) 与 普通物品 (cfu=Item)
        cfz dirtStack = new cfz(new cds());
        cfz swordStack = new cfz(new cfu());

        System.out.println("test(vanilla1201): 阶段1 未瞄准+空手 (canAttack=0 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.bN(dirtStack);
        mc.w = new eef(new bfz());
        System.out.println("test(vanilla1201): 阶段2 瞄准生物+手持BlockItem (canAttack=1 canPlace=1) 4 秒...");
        Thread.sleep(4000);

        player.bN(swordStack);
        System.out.println("test(vanilla1201): 阶段3 瞄准生物+手持普通Item (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.bN(null);
        System.out.println("test(vanilla1201): 阶段4 瞄准生物+空手 (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.bN(dirtStack);
        mc.w = null;
        System.out.println("test(vanilla1201): 阶段5 未瞄准+手持BlockItem (canAttack=0 canPlace=1) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(vanilla1201): 结束");
        System.exit(0);
    }
}
