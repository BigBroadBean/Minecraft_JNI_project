// 端到端测试: 模拟原版 1.8.9 (混淆运行时) 的 "Minecraft 1.8.9" 窗口
import javax.swing.JFrame;

public class TestMC189V {
    public static void main(String[] args) throws Exception {
        ave mc = ave.A();
        mc.h = new bew();

        JFrame frame = new JFrame("Minecraft 1.8.9");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        System.out.println("test(vanilla189): 阶段1 未瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        mc.s = new auh(new pr());
        System.out.println("test(vanilla189): 阶段2 瞄准生物 (canAttack 应为 1) 10 秒...");
        Thread.sleep(10000);

        mc.s = null;
        System.out.println("test(vanilla189): 阶段3 取消瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(vanilla189): 结束");
        System.exit(0);
    }
}
