// 测试用假客户端 (MCP 命名结构), 用于在无游戏的情况下验证注入链路
package net.minecraft.util;

import net.minecraft.entity.Entity;

public class MovingObjectPosition {
    public enum MovingObjectType { MISS, BLOCK, ENTITY }

    public MovingObjectType typeOfHit;
    public Entity entityHit;

    public MovingObjectPosition() {
        this.typeOfHit = MovingObjectType.MISS;
    }

    public MovingObjectPosition(Entity entityHit, MovingObjectType typeOfHit) {
        this.entityHit = entityHit;
        this.typeOfHit = typeOfHit;
    }
}
