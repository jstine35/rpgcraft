﻿using UnityEngine;
using System.Collections;
using System.Collections.Generic;

/// <summary>
/// High-level class for collision system
/// </summary>
public class CollisionManager : MonoSingleton<CollisionManager> 
{
    public bool HasCollision(ChunkInfo ci, int x, int y)
    {
        Vector2 worldPos = GameManager.Chunk2World(ci, x, y);

        Box2D worldBox = new Box2D(worldPos, 0.5f, 0.5f);

        // Check against any player
        Box2D playerBox = GameManager.Instance.MainPlayer.Box;

        return CollisionCode.TestBox2DBox2D(worldBox, playerBox);
    }

    public bool EnemyWithinPlayerRadius(float radius)
    {
        var playerPos = EntityManager.Instance.Player.transform.position;

        foreach (var entity in EntityManager.Instance.Entities)
        {
            if (!(entity is Enemy))
            {
                continue;
            }

            var entityPos = entity.transform.position;
            if ((entityPos - playerPos).sqrMagnitude <= radius * radius)
            {
                return true;
            }
        }

        return false;
    }

    static Vector2[] neighbors = {
        new Vector2(0, 0),
        new Vector2(-1, 1),
        new Vector2(0, 1),
        new Vector2(1, 1),
        new Vector2(-1, 0),
        new Vector2(1, 0),
        new Vector2(-1, -1),
        new Vector2(0, -1),
        new Vector2(1, -1),
    };

    public void OnLateUpdate(Entity entity, Vector2 lastPosition, Vector2 newPosition)
    {
        // Need to check all 8 neighbors, including yourself.
        // TODO: This assumes the entity is not bigger than a single cell
        if (lastPosition == newPosition)
        {
            return;
        }

        for (int i = 0; i < 9; i++)
        {
            ChunkInfo chunkInfo;
            int x, y;
            GameManager.Instance.GetTileDataFromWorldPos(lastPosition + neighbors[i], out chunkInfo, out x, out y);

            if (chunkInfo != null)
            {
                chunkInfo.RemoveEntity(entity, x, y);
            }
        }

        for (int i = 0; i < 9; i++)
        {
            ChunkInfo chunkInfo;
            int x, y;
            GameManager.Instance.GetTileDataFromWorldPos(newPosition + neighbors[i], out chunkInfo, out x, out y);

            if (chunkInfo != null)
            {
                chunkInfo.AddEntity(entity, x, y);
            }
        }
    }

    /// <summary>
    /// Return iterator for all entities within a certain radius of another entity.
    /// Doesn't return itself
    /// </summary>
    /// <param name="source">Source entity</param>
    /// <param name="radius">Radius for checking</param>
    /// <returns></returns>
    public IEnumerator<Entity> EntitiesWithinEntityRadius(Entity source, float radius)
    {
        if (source == null || radius <= 0)
        {
            yield break;
        }
        
        foreach (var entity in EntityManager.Instance.Entities)
        {
            if (entity == source)
            {
                continue;
            }

            var entityPos = entity.transform.position;
            if ((entityPos - source.transform.position).sqrMagnitude <= radius * radius)
            {
                yield return entity;
            }
        }
    }
}
