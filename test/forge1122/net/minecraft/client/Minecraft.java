// 测试用假客户端 (Forge 1.12.2 SRG 运行时, MCP 类名)
// 模拟: net.minecraft.client.Minecraft + func_71410_x + field_71439_g + field_71476_x
//       RayTraceResult (1.12 从 MovingObjectPosition 改名!)
//       func_184614_ca = getHeldItemMainhand (1.9+ 双持系统, 无参 getHeldItem 已移除)
package net.minecraft.client;

import net.minecraft.client.entity.EntityPlayerSP;
import net.minecraft.util.math.RayTraceResult;

public class Minecraft {
    public static Minecraft S = new Minecraft();
    public EntityPlayerSP field_71439_g;   // thePlayer
    public RayTraceResult field_71476_x;   // objectMouseOver

    public static Minecraft func_71410_x() { // getMinecraft
        return S;
    }
}
