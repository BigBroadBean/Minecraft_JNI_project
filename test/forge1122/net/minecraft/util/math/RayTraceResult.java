// 测试用假客户端 (Forge 1.12.2 SRG 运行时, MCP 类名)
// 1.12 的 RayTraceResult (MovingObjectPosition 改名)
package net.minecraft.util.math;

import net.minecraft.entity.Entity;

public class RayTraceResult {
    public enum Type { MISS, BLOCK, ENTITY }

    public Type field_72313_a;       // typeOfHit
    public Entity field_72308_g;     // entityHit

    public RayTraceResult() {
        this.field_72313_a = Type.MISS;
    }

    public RayTraceResult(Entity entityHit, Type typeOfHit) {
        this.field_72308_g = entityHit;
        this.field_72313_a = typeOfHit;
    }
}
