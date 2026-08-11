// 端到端测试: 模拟原版 1.8.9 (混淆运行时) 的 "Minecraft 1.8.9" 窗口
// 判定预期:
//   canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
//   canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
import javax.swing.JFrame;

public class TestMC189V {
    public static void main(String[] args) throws Exception {
        ave mc = ave.A();
        bew player = new bew();
        mc.h = player;

        JFrame frame = new JFrame("Minecraft 1.8.9");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        // 物品: 放置物 (yo=ItemBlock) 与 普通物品 (zw=Item)
        zx dirtStack = new zx(new yo());
        zx swordStack = new zx(new zw());

        System.out.println("test(vanilla189): 阶段1 未瞄准+空手 (canAttack=0 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.bB(dirtStack);
        mc.s = new auh(new pr());
        System.out.println("test(vanilla189): 阶段2 瞄准生物+手持ItemBlock (canAttack=1 canPlace=1) 4 秒...");
        Thread.sleep(4000);

        player.bB(swordStack);
        System.out.println("test(vanilla189): 阶段3 瞄准生物+手持普通Item (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.bB(null);
        System.out.println("test(vanilla189): 阶段4 瞄准生物+空手 (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.bB(dirtStack);
        mc.s = null;
        System.out.println("test(vanilla189): 阶段5 未瞄准+手持ItemBlock (canAttack=0 canPlace=1) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(vanilla189): 结束");
        System.exit(0);
    }
}
