// 端到端测试: 模拟 Forge/NeoForge 1.20.1 (Mojang 官方名) 的 "Minecraft* 1.20.1" 窗口
import javax.swing.JFrame;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.EntityHitResult;

public class TestMC1201F {
    public static void main(String[] args) throws Exception {
        Minecraft mc = Minecraft.getInstance();
        mc.player = new LocalPlayer();

        JFrame frame = new JFrame("Minecraft* 1.20.1");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        System.out.println("test(forge1201): 阶段1 未瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        mc.hitResult = new EntityHitResult(new LivingEntity());
        System.out.println("test(forge1201): 阶段2 瞄准生物 (canAttack 应为 1) 10 秒...");
        Thread.sleep(10000);

        mc.hitResult = null;
        System.out.println("test(forge1201): 阶段3 取消瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(forge1201): 结束");
        System.exit(0);
    }
}
