// 端到端测试: 模拟 Forge/NeoForge 1.20.1 (Mojang 官方名) 的 "Minecraft* 1.20.1" 窗口
// 判定预期:
//   canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
//   canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
import javax.swing.JFrame;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.phys.EntityHitResult;

public class TestMC1201F {
    public static void main(String[] args) throws Exception {
        Minecraft mc = Minecraft.getInstance();
        LocalPlayer player = new LocalPlayer();
        mc.player = player;

        JFrame frame = new JFrame("Minecraft* 1.20.1");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        // 物品: 放置物 (BlockItem) 与 普通物品 (Item)
        ItemStack dirtStack = new ItemStack(new BlockItem());
        ItemStack swordStack = new ItemStack(new Item());

        System.out.println("test(forge1201): 阶段1 未瞄准+空手 (canAttack=0 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.hitResult = new EntityHitResult(new LivingEntity());
        System.out.println("test(forge1201): 阶段2 瞄准生物+手持BlockItem (canAttack=1 canPlace=1) 4 秒...");
        Thread.sleep(4000);

        player.setHeldItem(swordStack);
        System.out.println("test(forge1201): 阶段3 瞄准生物+手持普通Item (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(null);
        System.out.println("test(forge1201): 阶段4 瞄准生物+空手 (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.hitResult = null;
        System.out.println("test(forge1201): 阶段5 未瞄准+手持BlockItem (canAttack=0 canPlace=1) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(forge1201): 结束");
        System.exit(0);
    }
}
