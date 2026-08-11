// 测试用假客户端 (MCP 命名结构)
package net.minecraft.entity;

import net.minecraft.item.ItemStack;

public class EntityLivingBase extends Entity {
    private ItemStack heldItem;

    public ItemStack getHeldItem() {
        return heldItem;
    }

    // 测试用: 设置手持物品
    public void setHeldItem(ItemStack stack) {
        this.heldItem = stack;
    }
}
