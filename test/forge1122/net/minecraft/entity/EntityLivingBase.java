// 测试用假客户端 (Forge 1.12.2 SRG 运行时, MCP 类名)
// 1.9+ 双持: 无参 getHeldItem(func_70694_bm) 已移除, 改用 getHeldItemMainhand(func_184614_ca)
package net.minecraft.entity;

import net.minecraft.item.ItemStack;

public class EntityLivingBase extends Entity {
    private ItemStack heldItemMainhand;

    public ItemStack func_184614_ca() { // getHeldItemMainhand
        return heldItemMainhand;
    }

    // 测试用: 设置手持物品 (非游戏名)
    public void setHeldItem(ItemStack stack) {
        this.heldItemMainhand = stack;
    }
}
