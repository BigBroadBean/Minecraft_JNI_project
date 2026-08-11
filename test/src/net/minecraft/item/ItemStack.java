// 测试用假客户端 (MCP 命名结构) — 1.8.9 ItemStack
package net.minecraft.item;

public class ItemStack {
    private Item item;

    public ItemStack(Item item) {
        this.item = item;
    }

    public Item getItem() {
        return item;
    }
}
