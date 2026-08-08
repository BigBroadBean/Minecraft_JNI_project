// 端到端测试: 模拟 Forge 1.8.9 (SRG 运行时) 的 "Minecraft 1.8.9" 窗口
import javax.swing.JFrame;

public class TestMC189F {
    public static void main(String[] args) throws Exception {
        ave mc = ave.func_71410_x();
        mc.field_71439_g = new bew();

        JFrame frame = new JFrame("Minecraft 1.8.9");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        System.out.println("test(forge189): 阶段1 未瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        mc.field_71476_x = new auh(new pr());
        System.out.println("test(forge189): 阶段2 瞄准生物 (canAttack 应为 1) 10 秒...");
        Thread.sleep(10000);

        mc.field_71476_x = null;
        System.out.println("test(forge189): 阶段3 取消瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(forge189): 结束");
        System.exit(0);
    }
}
