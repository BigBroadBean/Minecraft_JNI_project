// 端到端测试: 模拟原版/Fabric 1.20.1 (混淆运行时) 的 "Minecraft* 1.20.1" 窗口
import javax.swing.JFrame;

public class TestMC1201V {
    public static void main(String[] args) throws Exception {
        enn mc = enn.N();
        mc.t = new fiy();

        JFrame frame = new JFrame("Minecraft* 1.20.1");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        System.out.println("test(vanilla1201): 阶段1 未瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        mc.w = new eef(new bfz());
        System.out.println("test(vanilla1201): 阶段2 瞄准生物 (canAttack 应为 1) 10 秒...");
        Thread.sleep(10000);

        mc.w = null;
        System.out.println("test(vanilla1201): 阶段3 取消瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(vanilla1201): 结束");
        System.exit(0);
    }
}
