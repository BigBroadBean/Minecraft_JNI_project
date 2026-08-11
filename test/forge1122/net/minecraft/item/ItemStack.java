// 测试用假客户端 (Forge 1.12.2 SRG 运行时, MCP 类名) — ItemStack
package net.minecraft.item;

public class ItemStack {
    private Item item;

    public ItemStack(Item item) {
        this.item = item;
    }

    public Item func_77973_b() { // getItem
        return item;
    }
}
