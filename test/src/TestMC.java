// 端到端测试: 模拟 "Minecraft 1.8.9" 窗口 + MCP 命名类结构
// 运行: java -cp out TestMC   (窗口标题为 "Minecraft 1.8.9")
// 判定预期:
//   canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
//   canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
import javax.swing.JFrame;
import net.minecraft.client.Minecraft;
import net.minecraft.client.entity.EntityPlayerSP;
import net.minecraft.entity.monster.EntityZombie;
import net.minecraft.item.Item;
import net.minecraft.item.ItemBlock;
import net.minecraft.item.ItemStack;
import net.minecraft.util.MovingObjectPosition;
import net.minecraft.util.MovingObjectPosition.MovingObjectType;

public class TestMC {
    public static void main(String[] args) throws Exception {
        Minecraft mc = Minecraft.getMinecraft();
        EntityPlayerSP player = new EntityPlayerSP();
        mc.thePlayer = player;

        JFrame frame = new JFrame("Minecraft 1.8.9");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        // 物品: 放置物 (ItemBlock) 与 普通物品 (Item)
        ItemStack dirtStack = new ItemStack(new ItemBlock());
        ItemStack swordStack = new ItemStack(new Item());
        EntityZombie zombie = new EntityZombie();

        System.out.println("test: 阶段1 未瞄准+空手 (canAttack=0 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.objectMouseOver = new MovingObjectPosition(zombie, MovingObjectType.ENTITY);
        System.out.println("test: 阶段2 瞄准僵尸+手持ItemBlock (canAttack=1 canPlace=1) 4 秒...");
        Thread.sleep(4000);

        player.setHeldItem(swordStack);
        System.out.println("test: 阶段3 瞄准僵尸+手持普通Item (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(null);
        System.out.println("test: 阶段4 瞄准僵尸+空手 (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.objectMouseOver = null;
        System.out.println("test: 阶段5 未瞄准+手持ItemBlock (canAttack=0 canPlace=1) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test: 结束");
        System.exit(0);
    }
}
