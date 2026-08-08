// 测试用假客户端 (MCP 命名结构), 用于在无游戏的情况下验证注入链路
package net.minecraft.client;

import net.minecraft.client.entity.EntityPlayerSP;
import net.minecraft.util.MovingObjectPosition;

public class Minecraft {
    public static Minecraft theMinecraft = new Minecraft();
    public EntityPlayerSP thePlayer;
    public MovingObjectPosition objectMouseOver;

    public static Minecraft getMinecraft() {
        return theMinecraft;
    }
}
