// 端到端测试: 模拟 "Minecraft 1.8.9" 窗口 + MCP 命名类结构
// 运行: java -cp out TestMC   (窗口标题为 "Minecraft 1.8.9")
import javax.swing.JFrame;
import net.minecraft.client.Minecraft;
import net.minecraft.client.entity.EntityPlayerSP;
import net.minecraft.entity.monster.EntityZombie;
import net.minecraft.util.MovingObjectPosition;
import net.minecraft.util.MovingObjectPosition.MovingObjectType;

public class TestMC {
    public static void main(String[] args) throws Exception {
        Minecraft mc = Minecraft.getMinecraft();
        mc.thePlayer = new EntityPlayerSP();

        JFrame frame = new JFrame("Minecraft 1.8.9");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        System.out.println("test: 阶段1 未瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        EntityZombie zombie = new EntityZombie();
        mc.objectMouseOver = new MovingObjectPosition(zombie, MovingObjectType.ENTITY);
        System.out.println("test: 阶段2 瞄准僵尸 (canAttack 应为 1) 10 秒...");
        Thread.sleep(10000);

        mc.objectMouseOver = null;
        System.out.println("test: 阶段3 取消瞄准 (canAttack 应为 0) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test: 结束");
        System.exit(0);
    }
}
