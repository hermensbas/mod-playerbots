/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TravelNode.h"
#include "PlayerbotsDatabase.h"
#include "BudgetValues.h"
#include "MapMgr.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "RaceMgr.h"
#include "ServerFacade.h"
#include "Transport.h"
#include "TransportMgr.h"
#include <array>
#include <iomanip>
#include <queue>
#include <regex>
#include <unordered_set>

// TravelNodePath(float distance = 0.1f, float extraCost = 0, TravelNodePathType pathType = TravelNodePathType::walk,
// uint32 pathObject = 0, bool calculated = false, std::vector<uint8> maxLevelCreature = { 0,0,0 }, float swimDistance =
// 0)
std::string const TravelNodePath::print()
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << distance << "f,";
    out << extraCost << "f,";
    out << std::to_string(uint8(pathType)) << ",";
    out << pathObject << ",";
    out << (calculated ? "true" : "false") << ",";
    out << std::to_string(maxLevelCreature[0]) << "," << std::to_string(maxLevelCreature[1]) << ","
        << std::to_string(maxLevelCreature[2]) << ",";
    out << swimDistance << "f";

    return out.str().c_str();
}

// Gets the extra information needed to properly calculate the cost.
void TravelNodePath::calculateCost(bool distanceOnly)
{
    std::unordered_map<FactionTemplateEntry const*, bool> aReact, hReact;

    bool aFriend, hFriend;

    if (calculated)
        return;

    distance = 0.1f;
    maxLevelCreature = {0, 0, 0};
    swimDistance = 0;

    WorldPosition lastPoint = WorldPosition();
    for (auto& point : path)
    {
        if (!distanceOnly)
        {
            for (CreatureData const* cData : point.getCreaturesNear(50))  // Agro radius + 5
            {
                CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(cData->id);
                if (cInfo)
                {
                    FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->faction);

                    if (aReact.find(factionEntry) == aReact.end())
                        aReact.insert(std::make_pair(
                            factionEntry, Unit::GetFactionReactionTo(
                                              factionEntry, sFactionTemplateStore.LookupEntry(1)) > REP_NEUTRAL));
                    aFriend = aReact.find(factionEntry)->second;

                    if (hReact.find(factionEntry) == hReact.end())
                        hReact.insert(std::make_pair(
                            factionEntry, Unit::GetFactionReactionTo(
                                              factionEntry, sFactionTemplateStore.LookupEntry(2)) > REP_NEUTRAL));
                    hFriend = hReact.find(factionEntry)->second;

                    if (maxLevelCreature[0] < cInfo->maxlevel && !aFriend && !hFriend)
                        maxLevelCreature[0] = cInfo->maxlevel;
                    if (maxLevelCreature[1] < cInfo->maxlevel && aFriend && !hFriend)
                        maxLevelCreature[1] = cInfo->maxlevel;
                    if (maxLevelCreature[2] < cInfo->maxlevel && !aFriend && hFriend)
                        maxLevelCreature[2] = cInfo->maxlevel;
                }
            }
        }

        if (lastPoint && point.GetMapId() == lastPoint.GetMapId())
        {
            if (!distanceOnly && (point.isInWater() || lastPoint.isInWater()))
                swimDistance += point.distance(lastPoint);

            distance += point.distance(lastPoint);
        }

        lastPoint = point;
    }

    if (!distanceOnly)
        calculated = true;
}

// The cost to travel this path.
float TravelNodePath::getCost(Player* bot, uint32 cGold)
{
    float modifier = 1.0f;  // Global modifier
    float timeCost = 0.1f;
    float runDistance = distance - swimDistance;
    float speed = 8.0f;      // default run speed
    float swimSpeed = 4.0f;  // default swim speed.

    if (bot)
    {
        if (getPathType() == TravelNodePathType::flightPath && pathObject)
        {
            if (!bot->IsAlive())
                return -1.0f;

            TaxiPathEntry const* taxiPath = sTaxiPathStore.LookupEntry(pathObject);

            if (!taxiPath)
                return -1.0f;

            if (!bot->isTaxiCheater() && taxiPath->price > cGold)
                return -1.0f;

            if (!bot->isTaxiCheater() && !bot->m_taxi.IsTaximaskNodeKnown(taxiPath->to))
                return -1.0f;

            TaxiNodesEntry const* startTaxiNode = sTaxiNodesStore.LookupEntry(taxiPath->from);
            TaxiNodesEntry const* endTaxiNode = sTaxiNodesStore.LookupEntry(taxiPath->to);
            if (!startTaxiNode || !endTaxiNode ||
                !startTaxiNode->MountCreatureID[bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0] ||
                !endTaxiNode->MountCreatureID[bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0])
                return -1.0f;
        }

        speed = bot->GetSpeed(MOVE_RUN);
        swimSpeed = bot->GetSpeed(MOVE_SWIM);

        if (bot->HasSpell(1066))
            swimSpeed *= 1.5;

        uint32 level = bot->GetLevel();
        bool isAlliance = Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(),
                                                     sFactionTemplateStore.LookupEntry(1)) > REP_NEUTRAL;

        int factionAnnoyance = 0;
        if (maxLevelCreature.size() > 0)
        {
            int mobAnnoyance = (maxLevelCreature[0] - level) - 10;  // Mobs 10 levels below do not bother us.

            if (isAlliance)
                factionAnnoyance = (maxLevelCreature[2] - level) - 10;  // Opposite faction below 30 do not bother us.
            else if (!isAlliance)
                factionAnnoyance = (maxLevelCreature[1] - level) - 10;

            if (mobAnnoyance > 0)
                modifier += 0.1 * mobAnnoyance;  // For each level the whole path takes 10% longer.
            if (factionAnnoyance > 0)
                modifier += 0.3 * factionAnnoyance;  // For each level the whole path takes 10% longer.
        }
        if (getPathType() == TravelNodePathType::flyingMount)
        {
            if (!bot->IsAlive() || bot->GetLevel() < 70 || !bot->CanFly())
                return -1.0f;

            float flySpeed = bot->GetSpeed(MOVE_FLIGHT);
            if (flySpeed < 1.0f)
                flySpeed = 20.0f;  // 280% base flying speed fallback
            return (distance / flySpeed) * modifier;
        }
    }
    else if (getPathType() == TravelNodePathType::flightPath || getPathType() == TravelNodePathType::flyingMount)
        return -1.0f;

    if (getPathType() != TravelNodePathType::walk)
        timeCost = extraCost * modifier;
    else
        timeCost = (runDistance / speed + swimDistance / swimSpeed) * modifier;

    return timeCost;
}

uint32 TravelNodePath::getPrice()
{
    if (getPathType() != TravelNodePathType::flightPath)
        return 0;

    if (!pathObject)
        return 0;

    TaxiPathEntry const* taxiPath = sTaxiPathStore.LookupEntry(pathObject);

    if (!taxiPath)
        return 0;

    return taxiPath->price;
}

// Creates or appends the path from one node to another. Returns if the path.
TravelNodePath* TravelNode::BuildPath(TravelNode* endNode, Unit* bot, bool postProcess)
{
    if (GetMapId() != endNode->GetMapId())
        return nullptr;

    TravelNodePath* returnNodePath;

    if (!hasPathTo(endNode))  // Create path if it doesn't exists
        returnNodePath = setPathTo(endNode, TravelNodePath(), false);
    else
        returnNodePath = getPathTo(endNode);  // Get the exsisting path.

    if (returnNodePath->getComplete())  // Path is already complete. Return it.
        return returnNodePath;

    std::vector<WorldPosition> path = returnNodePath->GetPath();

    if (path.empty())
        path = {*getPosition()};  // Start the path from the current Node.

    WorldPosition* endPos = endNode->getPosition();  // Build the path to the end Node.

    path = endPos->getPathFromPath(path, bot);  // Pathfind from the existing path to the end Node.

    bool canPath = endPos->isPathTo(path);  // Check if we reached our destination.

    // Walk → portal/transport cheat: forward stalled but we got within
    // 20y of the dest. Add a midpoint waypoint (if the gap is >1y) plus
    // the endpoint and accept. Must run before the IsPathCheating 2-point
    // reject so the appended points lift size above 2.
    if (!canPath && !isTransport() && !isPortal() &&
        (endNode->isPortal() || endNode->isTransport()))
    {
        if (endPos->isPathTo(path, 20.0f))
        {
            if (path.back().distance(endPos) > 1.0f)
            {
                float mx = (endPos->GetPositionX() + path.back().GetPositionX()) * 0.5f;
                float my = (endPos->GetPositionY() + path.back().GetPositionY()) * 0.5f;
                float mz = (endPos->GetPositionZ() + path.back().GetPositionZ()) * 0.5f;
                path.emplace_back(endPos->GetMapId(), mx, my, mz);
            }
            path.push_back(*endPos);
            canPath = true;
        }
    }

    // Reject too-short or too-steep results — geometry shortcut that
    // mmap returns but a player can't actually walk.
    if (canPath && TravelPath::IsPathCheating(path, getPosition()->distance(endNode->getPosition())))
        canPath = false;

    // Persist the partial forward attempt before we try the reverse —
    // the recursive endNode->BuildPath below may itself check our state.
    returnNodePath->setPath(path);
    returnNodePath->setComplete(canPath);

    // Ensure the reverse path exists, recursively building it if needed.
    // The recursion is bounded: BuildPath returns immediately when the
    // reverse path is already marked complete.
    TravelNodePath* backNodePath = nullptr;
    if (!endNode->hasPathTo(this))
        backNodePath = endNode->BuildPath(this, bot, postProcess);
    else
        backNodePath = endNode->getPathTo(this);

    // Forward attempt failed — try to salvage with the reverse:
    //   * if the reverse is complete, flip it and use it
    //   * if the reverse is also partial but the two partials end near
    //     each other (<5y), stitch them into one path
    if (!canPath && backNodePath)
    {
        std::vector<WorldPosition> backPath = backNodePath->GetPath();
        if (!backPath.empty())
        {
            if (backNodePath->getComplete())
            {
                std::reverse(backPath.begin(), backPath.end());
                path = backPath;
                canPath = true;
            }
            else if (!path.empty() && path.back().distance(&backPath.back()) < 5.0f)
            {
                std::reverse(backPath.begin(), backPath.end());
                path.insert(path.end(), backPath.begin(), backPath.end());
                canPath = true;
            }
        }
    }

    if (isTransport() && path.size() > 1)
    {
        WorldPosition secondPos =
            *std::next(path.begin());  // This is to prevent bots from jumping in the water from a transport. Need to
                                       // remove this when transports are properly handled.
        if (secondPos.getMap() && secondPos.isInWater())
            canPath = false;
    }

    returnNodePath->setComplete(canPath);

    if (canPath && !hasLinkTo(endNode))
        setLinkTo(endNode, true);

    returnNodePath->setPath(path);

    if (!returnNodePath->getCalculated())
    {
        returnNodePath->calculateCost(!postProcess);
    }

    if (canPath && endNode->hasPathTo(this) && !endNode->hasLinkTo(this))
    {
        TravelNodePath* backNodePath = endNode->getPathTo(this);

        std::vector<WorldPosition> reversePath = path;
        reverse(reversePath.begin(), reversePath.end());
        backNodePath->setPath(reversePath);
        endNode->setLinkTo(this, true);

        if (!backNodePath->getCalculated())
        {
            backNodePath->calculateCost(!postProcess);
        }
    }

    return returnNodePath;
}

// Generic routine to remove references to nodes.
void TravelNode::removeLinkTo(TravelNode* node, bool removePaths)
{
    if (node)  // Unlink this specific node
    {
        if (removePaths)
            paths.erase(node);

        links.erase(node);
        routes.erase(node);
    }
    else
    {
        // Remove all references to this node.
        for (auto& node : TravelNodeMap::instance().getNodes())
        {
            if (node->hasPathTo(this))
                node->removeLinkTo(this, removePaths);
        }
        links.clear();
        paths.clear();
        routes.clear();
    }
}

std::vector<TravelNode*> TravelNode::getNodeMap(bool importantOnly, std::vector<TravelNode*> ignoreNodes)
{
    std::vector<TravelNode*> openList;
    std::vector<TravelNode*> closeList;

    openList.push_back(this);

    uint32 i = 0;

    while (i < openList.size())
    {
        TravelNode* currentNode = openList[i];

        i++;

        if (!importantOnly || currentNode->isImportant())
            closeList.push_back(currentNode);

        for (auto& nextPath : *currentNode->getLinks())
        {
            TravelNode* nextNode = nextPath.first;
            if (std::find(openList.begin(), openList.end(), nextNode) == openList.end())
            {
                if (ignoreNodes.empty() ||
                    std::find(ignoreNodes.begin(), ignoreNodes.end(), nextNode) == ignoreNodes.end())
                    openList.push_back(nextNode);
            }
        }
    }

    return closeList;
}

bool TravelNode::isUselessLink(TravelNode* farNode)
{
    if (getPathTo(farNode)->getPathType() != TravelNodePathType::walk)
        return false;

    float farLength;
    if (hasLinkTo(farNode))
        farLength = getPathTo(farNode)->getDistance();
    else
        farLength = getDistance(farNode);

    for (auto& link : *getLinks())
    {
        TravelNode* nearNode = link.first;
        float nearLength = link.second->getDistance();

        if (farNode == nearNode)
            continue;

        if (farNode->hasLinkTo(this) && !nearNode->hasLinkTo(this))
            continue;

        if (nearNode->hasLinkTo(farNode))
        {
            // Is it quicker to go past second node to reach first node instead of going directly?
            if (nearLength + nearNode->linkDistanceTo(farNode) < farLength * 1.1)
                return true;
        }
        else
        {
            TravelNodeRoute route = TravelNodeMap::instance().GetNodeRoute(nearNode, farNode, nullptr);

            if (route.isEmpty())
                continue;

            if (route.hasNode(this))
                continue;

            // Is it quicker to go past second (and multiple) nodes to reach the first node instead of going directly?
            if (nearLength + route.getTotalDistance() < farLength * 1.1)
                return true;
        }
    }

    return false;
}

void TravelNode::cropUselessLink(TravelNode* farNode)
{
    if (isUselessLink(farNode))
        removeLinkTo(farNode);
}

bool TravelNode::cropUselessLinks()
{
    bool hasRemoved = false;

    for (auto& firstLink : *getPaths())
    {
        TravelNode* farNode = firstLink.first;
        if (this->hasLinkTo(farNode) && this->isUselessLink(farNode))
        {
            this->removeLinkTo(farNode);
            hasRemoved = true;

            if (sPlayerbotAIConfig.HasLog("crop.csv"))
            {
                std::ostringstream out;
                out << getName() << ",";
                out << farNode->getName() << ",";
                WorldPosition().printWKT({*getPosition(), *farNode->getPosition()}, out, 1);
                out << std::fixed;

                sPlayerbotAIConfig.Log("crop.csv", out.str().c_str());
            }
        }

        if (farNode->hasLinkTo(this) && farNode->isUselessLink(this))
        {
            farNode->removeLinkTo(this);
            hasRemoved = true;

            if (sPlayerbotAIConfig.HasLog("crop.csv"))
            {
                std::ostringstream out;
                out << getName() << ",";
                out << farNode->getName() << ",";
                WorldPosition().printWKT({*getPosition(), *farNode->getPosition()}, out, 1);
                out << std::fixed;

                sPlayerbotAIConfig.Log("crop.csv", out.str().c_str());
            }
        }
    }

    return hasRemoved;

    /*

    //std::vector<std::pair<TravelNode*, TravelNode*>> toRemove;
    for (auto& firstLink : getLinks())
    {

        TravelNode* firstNode = firstLink.first;
        float firstLength = firstLink.second.getDistance();
        for (auto& secondLink : getLinks())
        {
            TravelNode* secondNode = secondLink.first;
            float secondLength = secondLink.second.getDistance();

            if (firstNode == secondNode)
                continue;

            if (std::find(toRemove.begin(), toRemove.end(), [firstNode, secondNode](std::pair<TravelNode*, TravelNode*>
    pair) {return pair.first == firstNode || pair.first == secondNode;}) != toRemove.end()) continue;

            if (firstNode->hasLinkTo(secondNode))
            {
                //Is it quicker to go past first node to reach second node instead of going directly?
                if (firstLength + firstNode->linkLengthTo(secondNode) < secondLength * 1.1)
                {
                    if (secondNode->hasLinkTo(this) && !firstNode->hasLinkTo(this))
                        continue;

                    toRemove.push_back(make_pair(this, secondNode));
                }
            }
            else
            {
                TravelNodeRoute route = TravelNodeMap::instance().GetNodeRoute(firstNode, secondNode, nullptr);

                if (route.isEmpty())
                    continue;

                if (route.hasNode(this))
                    continue;

                //Is it quicker to go past first (and multiple) nodes to reach the second node instead of going
    directly? if (firstLength + route.getLength() < secondLength * 1.1)
                {
                    if (secondNode->hasLinkTo(this) && !firstNode->hasLinkTo(this))
                        continue;

                    toRemove.push_back(make_pair(this, secondNode));
                }
            }
        }

        //Reverse cleanup. This is needed when we add a node in an existing map.
        if (firstNode->hasLinkTo(this))
        {
            firstLength = firstNode->getPathTo(this)->getDistance();

            for (auto& secondLink : firstNode->getLinks())
            {
                TravelNode* secondNode = secondLink.first;
                float secondLength = secondLink.second.getDistance();

                if (this == secondNode)
                    continue;

                if (std::find(toRemove.begin(), toRemove.end(), [firstNode, secondNode](std::pair<TravelNode*,
    TravelNode*> pair) {return pair.first == firstNode || pair.first == secondNode; }) != toRemove.end()) continue;

                if (firstNode->hasLinkTo(secondNode))
                {
                    //Is it quicker to go past first node to reach second node instead of going directly?
                    if (firstLength + firstNode->linkLengthTo(secondNode) < secondLength * 1.1)
                    {
                        if (secondNode->hasLinkTo(this) && !firstNode->hasLinkTo(this))
                            continue;

                        toRemove.push_back(make_pair(this, secondNode));
                    }
                }
                else
                {
                    TravelNodeRoute route = TravelNodeMap::instance().GetNodeRoute(firstNode, secondNode, nullptr);

                    if (route.isEmpty())
                        continue;

                    if (route.hasNode(this))
                        continue;

                    //Is it quicker to go past first (and multiple) nodes to reach the second node instead of going
    directly? if (firstLength + route.getLength() < secondLength * 1.1)
                    {
                        if (secondNode->hasLinkTo(this) && !firstNode->hasLinkTo(this))
                            continue;

                        toRemove.push_back(make_pair(this, secondNode));
                    }
                }
            }
        }

    }

    for (auto& nodePair : toRemove)
        nodePair.first->unlinkNode(nodePair.second, false);
        */
}

bool TravelNode::isEqual(TravelNode* compareNode)
{
    if (!hasLinkTo(compareNode))
        return false;

    if (!compareNode->hasLinkTo(this))
        return false;

    for (auto& node : TravelNodeMap::instance().getNodes())
    {
        if (node == this || node == compareNode)
            continue;

        if (node->hasLinkTo(this) != node->hasLinkTo(compareNode))
            return false;

        if (hasLinkTo(node) != compareNode->hasLinkTo(node))
            return false;
    }

    return true;
}

void TravelNode::print([[maybe_unused]] bool printFailed)
{
    // WorldPosition* startPosition = getPosition(); //not used, line marked for removal.

    uint32 mapSize = getNodeMap(true).size();

    std::ostringstream out;
    std::string name = getName();
    name.erase(std::remove(name.begin(), name.end(), '\"'), name.end());
    out << name.c_str() << ",";
    out << std::fixed << std::setprecision(2);
    point.printWKT(out);
    out << getZ() << ",";
    out << getO() << ",";
    out << (isImportant() ? 1 : 0) << ",";
    out << mapSize;

    sPlayerbotAIConfig.Log("travelNodes.csv", out.str().c_str());

    std::vector<WorldPosition> ppath;

    for (auto& endNode : TravelNodeMap::instance().getNodes())
    {
        if (endNode == this)
            continue;

        if (!hasPathTo(endNode))
            continue;

        TravelNodePath* path = getPathTo(endNode);

        if (!hasLinkTo(endNode) && urand(0, 20) && !printFailed)
            continue;

        ppath = path->GetPath();

        if (ppath.size() < 2 && hasLinkTo(endNode))
        {
            ppath.push_back(point);
            ppath.push_back(*endNode->getPosition());
        }

        if (ppath.size() > 1)
        {
            std::ostringstream out;

            uint32 pathType = static_cast<uint32>(path->getPathType());
            if (!hasLinkTo(endNode))
                pathType = 0;
            else if (!path->getComplete())
                pathType = 0;

            out << pathType << ",";
            out << std::fixed << std::setprecision(2);
            point.printWKT(ppath, out, 1);
            out << path->getPathObject() << ",";
            out << path->getDistance() << ",";
            out << path->getCost() << ",";
            out << (path->getComplete() ? 0 : 1) << ",";
            out << std::to_string(path->getMaxLevelCreature()[0]) << ",";
            out << std::to_string(path->getMaxLevelCreature()[1]) << ",";
            out << std::to_string(path->getMaxLevelCreature()[2]);

            sPlayerbotAIConfig.Log("travelPaths.csv", out.str().c_str());
        }
    }
}

// Attempts to move ahead of the path.
bool TravelPath::IsPathCheating(std::vector<WorldPosition> const& path, float endpointDistance)
{
    if (path.empty())
        return false;

    // Guard 1: 2-point path for >5y is navmesh "gave up" — straight
    // line through whatever's between A and B.
    if (path.size() == 2 && endpointDistance > 5.0f)
        return true;

    // Guard 2: steep slope at start or end suggests the pathfinder
    // hopped through a near-vertical step. >10y drop with >2:1 slope
    // is too steep to walk.
    if (path.size() > 2)
    {
        WorldPosition const& a = path.front();
        WorldPosition const& b = path[1];
        float vDist = std::fabs(a.GetPositionZ() - b.GetPositionZ());
        float hDist = a.GetExactDist2d(b.GetPositionX(), b.GetPositionY());
        if (vDist > 10.0f && (hDist == 0.0f || vDist / hDist > 2.0f))
            return true;

        WorldPosition const& c = path.back();
        WorldPosition const& d = path[path.size() - 2];
        float vDist2 = std::fabs(c.GetPositionZ() - d.GetPositionZ());
        float hDist2 = c.GetExactDist2d(d.GetPositionX(), d.GetPositionY());
        if (vDist2 > 10.0f && (hDist2 == 0.0f || vDist2 / hDist2 > 2.0f))
            return true;
    }

    return false;
}

bool TravelPath::makeShortCut(WorldPosition startPos, float maxDist, Unit* bot)
{
    if (fullPath.empty())
        return false;

    float maxDistSq = maxDist * maxDist;
    float minDist = -1;
    float totalDist = fullPath.begin()->point.sqDistance(startPos);
    std::vector<PathNodePoint> newPath;
    WorldPosition firstNode;

    for (auto& p : fullPath)  // cycle over the full path
    {
        // Walkability filter: portals/transports/taxis aren't valid
        // anchor points — picking one as the new start of the trimmed
        // path would leave the bot anchored on a hop.
        if (p.point.GetMapId() == startPos.GetMapId() && p.isWalkable())
        {
            float curDist = p.point.sqDistance(startPos);

            if (&p != &fullPath.front())
                totalDist += p.point.sqDistance(std::prev(&p)->point);

            if (curDist <
                sPlayerbotAIConfig.TooCloseDistance *
                    sPlayerbotAIConfig.TooCloseDistance)  // We are on the path. This is a good starting point
            {
                minDist = curDist;
                totalDist = curDist;
                newPath.clear();
            }

            if (p.type != PathNodeType::NODE_PREPATH)  // Only look at the part after the first node and in the same map.
            {
                if (!firstNode)
                    firstNode = p.point;

                if (minDist == -1 || curDist < minDist ||
                    (curDist < maxDistSq && curDist < totalDist / 2))  // Start building from the last closest point or
                                                                       // a point that is close but far on the path.
                {
                    minDist = curDist;
                    totalDist = curDist;
                    newPath.clear();
                }
            }
        }

        newPath.push_back(p);
    }

    if (newPath.empty() || minDist > maxDistSq || newPath.front().point.GetMapId() != startPos.GetMapId())
    {
        clear();
        return false;
    }

    WorldPosition beginPos = newPath.begin()->point;

    // The old path seems to be the best — either the closest walkable
    // point IS the original front, or it's within tooCloseDistance.
    if (newPath.front() == fullPath.front() ||
        beginPos.distance(firstNode) < sPlayerbotAIConfig.TooCloseDistance)
        return false;

    // We are (nearly) on the new path. Just follow the rest.
    if (beginPos.distance(startPos) < sPlayerbotAIConfig.TooCloseDistance)
    {
        fullPath = newPath;
        return true;
    }

    // Pass the bot into getPathTo so PathGenerator picks up its
    // collision/swim/fly state. nullptr defaults to a generic mover
    // which can produce paths the bot can't actually walk.
    std::vector<WorldPosition> toPath = startPos.getPathTo(beginPos, bot);

    // We can not reach the new begin position. Follow the complete path.
    if (!beginPos.isPathTo(toPath))
        return false;

    // Move to the new path and continue.
    fullPath.clear();
    addPath(toPath);
    addPath(newPath);

    return true;
}

std::ostringstream const TravelPath::print()
{
    std::ostringstream out;

    out << sPlayerbotAIConfig.GetTimestampStr();
    out << "+00,"
        << "1,";
    out << std::fixed;

    WorldPosition().printWKT(getPointPath(), out, 1);

    return out;
}

// Ported from cmangos TravelNode.cpp:804. Erase everything before (or up to
// and including) the given point. Returns false if the point isn't on the path.
bool TravelPath::cutTo(PathNodePoint point, bool including)
{
    auto it = std::find(fullPath.begin(), fullPath.end(), point);

    if (it != fullPath.end())
    {
        auto cutIt = including ? std::next(it) : it;

        if (cutIt != fullPath.begin())
            if (std::prev(cutIt)->type == PathNodeType::NODE_FLIGHTPATH)
                ASSERT(cutIt->type != PathNodeType::NODE_FLIGHTPATH ||
                       cutIt->entry != std::prev(cutIt)->entry);

        fullPath.erase(fullPath.begin(), cutIt);
        return true;
    }

    return false;
}

// Ported from cmangos TravelNode.cpp:905. Decide whether the walker should keep
// advancing past point p towards the next point.
bool TravelPath::shouldMoveToNextPoint(WorldPosition startPos,
                                       std::vector<PathNodePoint>::iterator beg,
                                       std::vector<PathNodePoint>::iterator ed,
                                       std::vector<PathNodePoint>::iterator p,
                                       float& moveDist, float maxDist)
{
    if (p == ed)  // We are the end. Stop now.
        return false;

    auto nextP = std::next(p);

    // Fix assertion fail due to nextP being invalidated.
    if (nextP == ed)  // We are the end. Stop now.
        return false;

    // We are moving to a area trigger node and want to move to the next teleport node.
    if (p->type == PathNodeType::NODE_AREA_TRIGGER && nextP->type == PathNodeType::NODE_AREA_TRIGGER &&
        p->entry == nextP->entry)
    {
        return false;  // Move to teleport and activate area trigger.
    }

    // We are moving to a static portal node and want to move to the next teleport node.
    if (p->type == PathNodeType::NODE_STATIC_PORTAL && nextP->type == PathNodeType::NODE_STATIC_PORTAL &&
        p->entry == nextP->entry)
    {
        return false;  // Move to teleport and activate the portal.
    }

    // We are using a hearthstone.
    if (p->type == PathNodeType::NODE_TELEPORT && nextP->type == PathNodeType::NODE_TELEPORT && p->entry == nextP->entry)
    {
        return false;  // Use the teleport
    }

    // We are almost at a transport node. Move to the node before this.
    if (nextP->type == PathNodeType::NODE_TRANSPORT && nextP->entry)
    {
        return false;
    }

    // We are moving to a transport node.
    if (p->type == PathNodeType::NODE_TRANSPORT && p->entry)
    {
        if (nextP->type != PathNodeType::NODE_TRANSPORT && p != beg &&
            std::prev(p)->type != PathNodeType::NODE_TRANSPORT)  // We are not using the transport. Skip it.
            return true;

        return false;  // Teleport to exit of transport.
    }

    // We are moving to a flightpath and want to fly.
    if (p->type == PathNodeType::NODE_FLIGHTPATH && nextP->type == PathNodeType::NODE_FLIGHTPATH)
    {
        return false;
    }

    float nextMove = p->point.distance(nextP->point);

    if (p->point.GetMapId() != startPos.GetMapId() ||
        ((moveDist + nextMove > maxDist || startPos.distance(nextP->point) > maxDist) && moveDist > 0))
    {
        return false;
    }

    moveDist += nextMove;

    return true;
}

// Ported from cmangos TravelNode.cpp:968. Next position to move to.
std::vector<PathNodePoint>::iterator TravelPath::getNextPoint(WorldPosition startPos, float maxDist, bool onTransport)
{
    float minDist = FLT_MAX;
    auto startP = fullPath.begin();

    if (!onTransport)
    {
        // Get the closest point on the path to start from.
        for (auto p = startP; p != fullPath.end(); p++)
        {
            if (p->point.GetMapId() != startPos.GetMapId())
                continue;

            float curDist = p->point.distance(startPos);

            if (!p->isWalkable())
                continue;

            if (curDist <= minDist)
            {
                minDist = curDist;
                startP = p;
            }
        }
    }

    if (startP == fullPath.end())
        return startP;

    float moveDist = startP->point.distance(startPos);

    // Move as far as we are allowed.
    for (auto p = startP; p != fullPath.end(); p++)
    {
        if (shouldMoveToNextPoint(startPos, fullPath.begin(), fullPath.end(), p, moveDist, maxDist))
            continue;

        startP = p;

        break;
    }

    if (startP == fullPath.end() || !startP->isWalkable())
        return startP;

    auto nextP = std::next(startP);

    if (nextP == fullPath.end())
        return startP;

    // If startPos is between startP and nextP we want to move to nextP instead.
    float project = startPos.projectOnSegment(startP->point, nextP->point);
    if (project > 0.0f && project < 1.0f)
        return nextP;

    return startP;
}

// Ported from cmangos TravelNode.cpp:1026. True when the upcoming leg is a
// special movement (area trigger / static portal / transport / flightpath /
// teleport); cuts the path to the activation point and returns true.
bool TravelPath::UpcommingSpecialMovement(WorldPosition startPos, float maxDist, bool onTransport)
{
    if (getPath().empty())
        return false;

    auto startP = getNextPoint(startPos, maxDist, onTransport);

    auto prevP = startP, nextP = startP;
    if (startP != fullPath.begin())
        prevP = std::prev(prevP);
    if (std::next(nextP) != fullPath.end())
        nextP = std::next(nextP);

    // We are moving towards an area trigger. Move to it and activate it.
    if (startP->type == PathNodeType::NODE_AREA_TRIGGER)
    {
        if (startP->entry)  // For area triggers we need to be close enough to trigger its activation.
        {
            // AC has no AreaTrigger.dbc store (sAreaTriggerStore/AreaTriggerEntry),
            // so validate the trigger via the server-side area-trigger teleport.
            // The point-in-box geometry check (DBC radius/box vs startPos) has no
            // AC equivalent and was already dead upstream, so it is omitted.
            AreaTrigger const* at = sObjectMgr->GetAreaTrigger(startP->entry);
            if (!at)
                return false;
        }

        cutTo(*startP, false);

        return true;
    }

    // We are moving towards a static portal. Move to it and use it.
    if (startP->type == PathNodeType::NODE_STATIC_PORTAL && startPos.distance(startP->point) < INTERACTION_DISTANCE)
    {
        cutTo(*startP, false);

        return true;
    }

    // We are using a hearthstone.
    if (nextP->type == PathNodeType::NODE_TELEPORT)
    {
        cutTo(*nextP, false);
        return true;
    }

    // We are moving towards a flight path. Move to the flight master and activate it.
    if (startP->type == PathNodeType::NODE_FLIGHTPATH && startPos.distance(startP->point) < INTERACTION_DISTANCE)
    {
        cutTo(*startP, false);
        return true;
    }

    // Walk on / teleport to transport.
    if (sPlayerbotAIConfig.TransportTeleportType < 2 && startP->type == PathNodeType::NODE_TRANSPORT)
    {
        uint32 entry = nextP->entry;

        if (!onTransport)
        {
            cutTo(*prevP, false);  // Previous point = dock, startP = where transport will stop.
            return true;
        }

        for (auto p = startP; p != fullPath.end(); p++)  // Move along the transport path to the end of the boat ride.
        {
            if (p->type != PathNodeType::NODE_TRANSPORT || (p->entry && p->entry != entry))
            {
                cutTo(*p, false);  // prevP = where transport will stop, startP = dock where we want to walk to.
                return true;
            }

            prevP = p;
        }
    }

    // Teleport to end of transport.
    if (sPlayerbotAIConfig.TransportTeleportType == 2 && nextP->type == PathNodeType::NODE_TRANSPORT)
    {
        for (auto p = startP + 1; p != fullPath.end(); p++)  // Move along the transport path to the end of the boat ride.
        {
            if (p->type != PathNodeType::NODE_TRANSPORT)
            {
                cutTo(*prevP, false);  // prevP = where transport will stop, startP = dock where we want to walk to.
                return true;
            }
        }
    }

    return false;
}

// Ported from cmangos TravelNode.cpp:1125. Clip the path at the first hazard /
// dangerous enemy / discontinuity so the bot stops short of trouble.
void TravelPath::ClipPath(PlayerbotAI* ai, Unit* mover, bool ignoreEnemyTargets)
{
    auto startP = getNextPoint(mover, 0.0f, false);

    // Check BEFORE cutTo: erase() invalidates startP, and the stale
    // iterator compares equal to the new end() exactly when the cut
    // prefix has as many points as the remainder — the early return
    // then fired mid-route and the WHOLE remaining path was dispatched
    // as one unclipped spline (the observed 243y dispatches). Latent in
    // the reference too; this restores its intended semantics.
    if (startP == fullPath.end())
        return;

    cutTo(*startP, false);

    if (fullPath.empty())
        return;

    // PORT-TODO: cmangos enemy-target + hazard clipping needs symbols absent in
    // modpb: PlayerbotAI::IsStateActive / BotState combat-state query, the
    // "possible attack targets" value (AI_VALUE_LAZY std::list<ObjectGuid>),
    // and the "hazards" value + HazardPosition type (AI_VALUE std::list<
    // HazardPosition>). These belong to the broader cmangos movement infra
    // (Phase B). Until they exist, only the distance/walkability/discontinuity
    // clipping below runs.
    //
    // ORIGINAL cmangos enemy/hazard pre-scan (kept verbatim for Phase B):
    //   AiObjectContext* context = ai->GetAiObjectContext();
    //   std::list<ObjectGuid> targets;
    //   if (!ai->IsStateActive(BotState::BOT_STATE_COMBAT) && !ai->GetBot()->isDead() && !ignoreEnemyTargets)
    //       targets = AI_VALUE_LAZY(std::list<ObjectGuid>, "possible attack targets");
    //   std::list<HazardPosition> hazards = AI_VALUE(std::list<HazardPosition>, "hazards");

    auto endP = fullPath.end();
    auto prevP = fullPath.begin();

    for (auto p = fullPath.begin(); p != fullPath.end(); p++)
    {
        // PORT-TODO (Phase B): per-point enemy-target scan and hazard scan go
        // here (see verbatim cmangos block above). Both depend on the missing
        // values/types noted above.

        if (p->point.sqDistance(fullPath.begin()->point) >
            sPlayerbotAIConfig.ReactDistance * sPlayerbotAIConfig.ReactDistance)
            endP = p;
        else if (!p->isWalkable())
            endP = p;
        else if (p->point.sqDistance(prevP->point) > 125)
        {
            endP = prevP;
        }

        if (endP != fullPath.end())
            break;

        prevP = p;
    }

    if (endP == fullPath.end())
        return;

    fullPath.erase(std::next(endP), fullPath.end());

    (void)ai;
    (void)mover;
    (void)ignoreEnemyTargets;
}

float TravelNodeRoute::getTotalDistance()
{
    if (nodes.size() < 2)
        return 0;

    float totalLength = 0;
    for (uint32 i = 0; i < nodes.size() - 1; i++)
        totalLength += nodes[i]->linkDistanceTo(nodes[i + 1]);

    return totalLength;
}

TravelPath TravelNodeRoute::BuildPath(std::vector<WorldPosition> pathToStart, std::vector<WorldPosition> pathToEnd,
                                      [[maybe_unused]] Unit* bot)
{
    TravelPath travelPath;

    if (!pathToStart.empty())  // From start position to start of path.
        travelPath.addPath(pathToStart, PathNodeType::NODE_PREPATH);

    TravelNode* prevNode = nullptr;
    for (auto& node : nodes)
    {
        if (prevNode)
        {
            TravelNodePath* nodePath = nullptr;
            if (prevNode->hasPathTo(node))  // Get the path to the next node if it exists.
                nodePath = prevNode->getPathTo(node);

            if (!nodePath || !nodePath->getComplete())  // Build the path to the next node if it doesn't exist.
            {
                // Only attempt runtime path building when we have a bot entity.
                if (bot)
                {
                    if (!prevNode->isTransport())
                        nodePath = prevNode->BuildPath(node, bot);
                    else
                    {
                        node->BuildPath(prevNode, bot);
                        nodePath = prevNode->getPathTo(node);
                    }
                }
            }

            TravelNodePath returnNodePath;

            if (!nodePath || !nodePath->getComplete())
            {
                if (bot)
                {
                    returnNodePath =
                        *node->BuildPath(prevNode, bot);
                    std::vector<WorldPosition> path = returnNodePath.GetPath();
                    std::reverse(path.begin(), path.end());
                    returnNodePath.setPath(path);
                    nodePath = &returnNodePath;
                }
            }

            if (!nodePath || !nodePath->getComplete())  // If we can not build a path just try to move to the node.
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_NODE);
                prevNode = node;
                continue;
            }

            // Phase B: the PathNodeType enum was flipped so value 3 is now
            // NODE_AREA_TRIGGER and value 7 is NODE_STATIC_PORTAL. Split what
            // was a combined NODE_PORTAL branch into the two distinct node
            // types the dispatch layer (HandleSpecialMovement) consumes:
            // TravelNodePathType::portal is an area-trigger teleport, while
            // staticPortal is a GO (spellcaster) teleport.
            if (nodePath->getPathType() == TravelNodePathType::portal)  // AreaTrigger teleport.
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_AREA_TRIGGER, nodePath->getPathObject());  // Entry point
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_AREA_TRIGGER, nodePath->getPathObject());      // Exit point
            }
            else if (nodePath->getPathType() == TravelNodePathType::staticPortal)  // GO portal teleport.
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_STATIC_PORTAL, nodePath->getPathObject());  // Entry point
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_STATIC_PORTAL, nodePath->getPathObject());      // Exit point
            }
            else if (nodePath->getPathType() == TravelNodePathType::transport)  // Move onto transport
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_TRANSPORT,
                                    nodePath->getPathObject());  // Departure point
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_TRANSPORT, nodePath->getPathObject());  // Arrival point
            }
            else if (nodePath->getPathType() == TravelNodePathType::flightPath)  // Use the flightpath
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_FLIGHTPATH,
                                    nodePath->getPathObject());  // Departure point
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_FLIGHTPATH, nodePath->getPathObject());  // Arrival point
            }
            else if (nodePath->getPathType() == TravelNodePathType::teleportSpell)
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_TELEPORT, nodePath->getPathObject());
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_TELEPORT, nodePath->getPathObject());
            }
            else if (nodePath->getPathType() == TravelNodePathType::flyingMount)
            {
                // Phase B: NODE_FLYING_MOUNT no longer exists (value 7 is now
                // NODE_STATIC_PORTAL). The source movement layer has no
                // flying-mount node type and the dispatch layer does not handle
                // one; flyingMount edges are "not currently enabled" per the
                // TravelNodePathType doc. Emit NODE_STATIC_PORTAL so this still
                // compiles; revisit when/if flying-mount routing is wired
                // (Phase C).
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_STATIC_PORTAL, 0);
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_STATIC_PORTAL, 0);
            }
            else
            {
                std::vector<WorldPosition> path = nodePath->GetPath();

                if (path.size() > 1 &&
                    node != nodes.back())  // Remove the last point since that will also be the start of the next path.
                    path.pop_back();

                if (path.size() > 1 && prevNode->isPortal() &&
                    nodePath->getPathType() != TravelNodePathType::portal &&
                    nodePath->getPathType() != TravelNodePathType::staticPortal)  // Do not move to the area trigger if we
                                                                                  // don't plan to take the portal.
                    path.erase(path.begin());

                if (path.size() > 1 && prevNode->isTransport() &&
                    nodePath->getPathType() !=
                        TravelNodePathType::transport)  // Do not move to the transport if we aren't going to take it.
                    path.erase(path.begin());

                travelPath.addPath(path, PathNodeType::NODE_PATH);
            }
        }
        prevNode = node;
    }

    if (!pathToEnd.empty())
        travelPath.addPath(pathToEnd, PathNodeType::NODE_PATH);

    return travelPath;
}

// Ported from cmangos TravelNode.cpp:1236. Faithful cmangos buildPath that
// emits NODE_AREA_TRIGGER / NODE_STATIC_PORTAL legs for the cmangos movement
// layer. Coexists with modpb's evolved BuildPath (capital B) above; modpb's is
// removed in Phase C once MoveTo2/DispatchMovement consume this output.
//
// NOTE (verified divergence): TravelNodePathType diverges between repos. modpb
// stores value 2 as `portal` (cmangos stores `areaTrigger`); modpb adds
// `flyingMount=7` which cmangos lacks. So cmangos's `== TravelNodePathType::
// areaTrigger` test is mapped to modpb's `== TravelNodePathType::portal` (same
// numeric link type, the SQL-stored entry-trigger edge), emitting
// NODE_AREA_TRIGGER points. This mirrors modpb BuildPath's portal handling.
TravelPath TravelNodeRoute::buildPath(std::vector<WorldPosition> pathToStart, std::vector<WorldPosition> pathToEnd,
                                      Unit* bot)
{
    TravelPath travelPath;

    Unit* botForPath = bot;

    if (!pathToStart.empty())  // From start position to start of path.
    {
        travelPath.addPath(pathToStart, PathNodeType::NODE_PREPATH);
    }

    TravelNode* prevNode = nullptr;
    for (auto& node : nodes)
    {
        if (prevNode)
        {
            TravelNodePath* nodePath = nullptr;
            if (prevNode->hasPathTo(node))  // Get the path to the next node if it exists.
                nodePath = prevNode->getPathTo(node);

            if (!nodePath || !nodePath->getComplete())  // Build the path to the next node if it doesn't exist.
            {
                // Runtime rebuilds mutate the shared node graph (setPathTo
                // map inserts) and route resolution only holds a shared
                // lock — so they run only when a mover is supplied. Both
                // resolvers pass nullptr; incomplete links degrade to a
                // plain move-to-node point below, which the walker handles.
                // Generation-time calls (unique lock) pass a mover.
                if (botForPath)
                {
                    if (!prevNode->isTransport())
                        nodePath = prevNode->BuildPath(node, botForPath);
                    else  // For transports we have no proper path since the node is in air/water. Instead we build a
                          // reverse path and follow that.
                    {
                        node->BuildPath(prevNode, botForPath);  // Reverse build to get proper path.
                        nodePath = prevNode->getPathTo(node);
                    }
                }
            }

            TravelNodePath returnNodePath;

            if (!nodePath || !nodePath->getComplete())  // It looks like we can't properly path to our node. Make a
                                                        // temporary reverse path and see if that works instead.
            {
                if (botForPath)
                {
                    if (TravelNodePath* reversePath = node->BuildPath(prevNode, botForPath))
                    {
                        returnNodePath = *reversePath;  // Build reverse path into a temporary.
                        std::vector<WorldPosition> path = returnNodePath.GetPath();
                        std::reverse(path.begin(), path.end());  // Reverse the path.
                        returnNodePath.setPath(path);
                        nodePath = &returnNodePath;
                    }
                }
                else if (!prevNode->isTransport() && prevNode->GetMapId() == node->GetMapId())
                {
                    // Null-mover leg repair: an incomplete link (a link
                    // row whose stored waypoints are missing) must not
                    // collapse to bare node centers — the dispatched
                    // spline interpolates STRAIGHT between them, walking
                    // the bot through the air for the whole gap. Path
                    // the leg live into a LOCAL temporary instead; no
                    // shared-graph mutation, so it stays safe under the
                    // shared lock the resolvers hold.
                    WorldPosition prevPos = *prevNode->getPosition();
                    WorldPosition nodePos = *node->getPosition();
                    std::vector<WorldPosition> localLeg = prevPos.getPathTo(nodePos, nullptr);
                    // Explicit tolerance: the default falls back to
                    // targetPosRecalcDistance (0.1y), which no navmesh
                    // path ever satisfies — the repair would never
                    // apply and the leg would stay a bare node point.
                    if (nodePos.isPathTo(localLeg, 2.0f))
                    {
                        returnNodePath = TravelNodePath(prevPos.distance(nodePos));
                        returnNodePath.setPath(localLeg);
                        returnNodePath.setComplete(true);
                        nodePath = &returnNodePath;
                    }
                }
            }

            if (!nodePath || !nodePath->getComplete())  // If we can not build a path just try to move to the node.
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_NODE);
            }
            else if (nodePath->getPathType() == TravelNodePathType::portal)  // entry-trigger edge -> area trigger.
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_AREA_TRIGGER,
                                    nodePath->getPathObject());  // Entry point
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_AREA_TRIGGER,
                                    nodePath->getPathObject());  // Exit point
            }
            else if (nodePath->getPathType() == TravelNodePathType::staticPortal)  // Teleport to next node.
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_STATIC_PORTAL,
                                    nodePath->getPathObject());  // Entry point
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_STATIC_PORTAL,
                                    nodePath->getPathObject());  // Exit point
            }
            else if (nodePath->getPathType() == TravelNodePathType::transport)  // Move onto transport
            {
                travelPath.addPath(nodePath->GetPath(), PathNodeType::NODE_TRANSPORT, nodePath->getPathObject());
            }
            else if (nodePath->getPathType() == TravelNodePathType::flightPath)  // Use the flightpath
            {
                travelPath.addPath(nodePath->GetPath(), PathNodeType::NODE_FLIGHTPATH, nodePath->getPathObject());
            }
            else if (nodePath->getPathType() == TravelNodePathType::teleportSpell)
            {
                travelPath.addPoint(*prevNode->getPosition(), PathNodeType::NODE_TELEPORT, nodePath->getPathObject());
                travelPath.addPoint(*node->getPosition(), PathNodeType::NODE_TELEPORT, nodePath->getPathObject());
            }
            else
            {
                std::vector<WorldPosition> path = nodePath->GetPath();

                if (path.size() > 1 && node != nodes.back())  // Remove the last point since that will also be the start
                                                              // of the next path.
                    path.pop_back();

                if (path.size() > 1 && prevNode->isPortal() &&
                    nodePath->getPathType() != TravelNodePathType::portal)  // Do not move to the area trigger if we
                                                                            // don't plan to take the portal.
                    path.erase(path.begin());

                if (path.size() > 1 && prevNode->isTransport() &&
                    nodePath->getPathType() != TravelNodePathType::transport)  // Do not move to the transport if we
                                                                               // aren't going to take it.
                    path.erase(path.begin());

                travelPath.addPath(path, PathNodeType::NODE_PATH);
            }
        }
        prevNode = node;
    }

    if (!pathToEnd.empty())
    {
        travelPath.addPath(pathToEnd, PathNodeType::NODE_PATH);
    }

    return travelPath;
}

std::ostringstream const TravelNodeRoute::print()
{
    std::ostringstream out;

    out << sPlayerbotAIConfig.GetTimestampStr();
    out << "+00"
        << ",0,"
        << "\"LINESTRING(";

    for (auto& node : nodes)
    {
        out << std::fixed << node->getPosition()->getDisplayX() << " " << node->getPosition()->getDisplayY() << ",";
    }

    out << ")\"";

    return out;
}

TravelNode* TravelNodeMap::addNode(WorldPosition pos, std::string const preferedName, bool isImportant,
                                   bool checkDuplicate, [[maybe_unused]] bool transport,
                                   [[maybe_unused]] uint32 transportId)
{
    TravelNode* newNode;

    if (checkDuplicate)
    {
        newNode = getNode(pos, nullptr, 5.0f);
        if (newNode)
            return newNode;
    }

    std::string finalName = preferedName;

    if (!isImportant)
    {
        std::regex last_num("[[:digit:]]+$");
        finalName = std::regex_replace(finalName, last_num, "");
        uint32 nameCount = 1;

        for (auto& node : getNodes())
        {
            if (node->getName().find(preferedName + std::to_string(nameCount)) != std::string::npos)
                nameCount++;
        }

        if (nameCount)
            finalName += std::to_string(nameCount);
    }

    newNode = new TravelNode(pos, finalName, isImportant);

    nodes.push_back(newNode);

    return newNode;
}

void TravelNodeMap::removeNode(TravelNode* node)
{
    node->removeLinkTo(nullptr, true);

    for (auto& tnode : nodes)
    {
        if (tnode == node)
        {
            delete tnode;
            tnode = nullptr;
        }
    }

    nodes.erase(std::remove(nodes.begin(), nodes.end(), nullptr), nodes.end());
}

void TravelNodeMap::fullLinkNode(TravelNode* startNode, Unit* bot)
{
    WorldPosition* startPosition = startNode->getPosition();
    std::vector<TravelNode*> linkNodes = getNodes(*startPosition);

    for (auto& endNode : linkNodes)
    {
        if (endNode == startNode)
            continue;

        if (startNode->hasLinkTo(endNode))
            continue;

        startNode->BuildPath(endNode, bot);
        endNode->BuildPath(startNode, bot);
    }

    startNode->setLinked(true);
}

std::vector<TravelNode*> TravelNodeMap::getNodes(WorldPosition pos, float range, uint32 transportEntry)
{
    std::vector<TravelNode*> retVec;

    for (auto& node : nodes)
    {
        if (node->GetMapId() == pos.GetMapId())
        {
            if (range != -1 && node->getDistance(pos) > range)
                continue;

            if (transportEntry && node->getTransportId() != transportEntry)
                continue;

            retVec.push_back(node);
        }
    }

    std::sort(retVec.begin(), retVec.end(),
              [pos](TravelNode* i, TravelNode* j)
              { return i->getPosition()->distance(pos) < j->getPosition()->distance(pos); });

    return retVec;
}

TravelNode* TravelNodeMap::getNode(WorldPosition pos, [[maybe_unused]] std::vector<WorldPosition>& ppath, Unit* bot,
                                   float range)
{
    //float x = pos.getX(); //not used, line marked for removal.
    //float y = pos.getY(); //not used, line marked for removal.
    //float z = pos.getZ(); //not used, line marked for removal.

    if (bot && !bot->GetMap())
        return nullptr;

    uint32 c = 0;

    std::vector<TravelNode*> nodes = TravelNodeMap::instance().getNodes(pos, range);
    for (auto& node : nodes)
    {
        if (!bot || pos.canPathTo(*node->getPosition(), bot))
            return node;

        c++;

        if (c > 5)  // Max 5 attempts
            break;
    }

    return nullptr;
}

TravelNodeRoute TravelNodeMap::GetNodeRoute(TravelNode* start, TravelNode* goal,
    Player* bot)
{
    float botSpeed = bot ? bot->GetSpeed(MOVE_RUN) : 7.0f;

    if (start == goal)
        return TravelNodeRoute();

    // Basic A* algorithm
    std::unordered_map<TravelNode*, TravelNodeStub> m_stubs;

    TravelNodeStub* startStub = &m_stubs.insert(std::make_pair(start, TravelNodeStub(start))).first->second;

    TravelNodeStub* currentNode = nullptr;
    TravelNodeStub* childNode = nullptr;
    float f = 0.f;
    float g = 0.f;
    float h = 0.f;

    std::vector<TravelNodeStub*> open, closed;

    if (bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI)
        {
            if (botAI->HasCheat(BotCheatMask::gold))
                startStub->currentGold = 10000000;
            else
            {
                AiObjectContext* context = botAI->GetAiObjectContext();
                startStub->currentGold = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::travel);
            }
        }
        else
            startStub->currentGold = bot->GetMoney();
    }

    if (!start->hasRouteTo(goal))
        return TravelNodeRoute();

    // Min-heap: smallest f at front
    auto heapComp = [](TravelNodeStub* i, TravelNodeStub* j) { return i->totalCost > j->totalCost; };

    open.push_back(startStub);
    std::push_heap(open.begin(), open.end(), heapComp);
    startStub->open = true;

    // Runaway guard only: with a valid heap a real route resolves in a
    // few dozen expansions; this exists to bound a pathological graph,
    // not to fail legitimate long routes (the old 500 cap did, because
    // in-place cost mutation corrupted the heap and exploration order).
    constexpr uint32 MAX_A_STAR_EXPLORED = 3000;
    uint32 nodesExplored = 0;

    while (!open.empty())
    {
        std::pop_heap(open.begin(), open.end(), heapComp);
        currentNode = open.back();
        open.pop_back();

        // Lazy deletion: relaxing a node already in the heap pushes a
        // duplicate entry instead of mutating in place (which would
        // corrupt heap order). A node expanded earlier at better cost
        // leaves stale entries behind — skip them.
        if (currentNode->closed)
            continue;

        if (++nodesExplored > MAX_A_STAR_EXPLORED)
            return TravelNodeRoute();

        currentNode->open = false;

        currentNode->closed = true;
        closed.push_back(currentNode);

        if (currentNode->dataNode == goal)
        {
            TravelNodeStub* parent = currentNode->parent;

            std::vector<TravelNode*> path;
            path.push_back(currentNode->dataNode);

            while (parent != nullptr)
            {
                path.push_back(parent->dataNode);
                parent = parent->parent;
            }

            reverse(path.begin(), path.end());

            return TravelNodeRoute(path);
        }

        for (auto const& link : *currentNode->dataNode->getLinks())  // for each successor n' of n
        {
            TravelNode* linkNode = link.first;
            float linkCost = link.second->getCost(bot, currentNode->currentGold);

            if (linkCost <= 0)
                continue;

            childNode = &m_stubs.insert(std::make_pair(linkNode, TravelNodeStub(linkNode))).first->second;
            g = currentNode->costFromStart + linkCost;  // stance from start + distance between the two nodes
            if ((childNode->open || childNode->closed) &&
                childNode->costFromStart <= g)  // n' is already in opend or closed with a lower cost g(n')
                continue;             // consider next successor

            h = childNode->dataNode->fDist(goal) / botSpeed;
            f = g + h; // compute f(n')
            childNode->totalCost = f;
            childNode->costFromStart = g;
            childNode->heuristic = h;
            childNode->parent = currentNode;

            if (bot && !bot->isTaxiCheater())
                childNode->currentGold = currentNode->currentGold - link.second->getPrice();

            if (childNode->closed)
                childNode->closed = false;

            // Always push on relax — duplicates are cheaper than a
            // corrupted heap; stale entries are skipped at pop time.
            open.push_back(childNode);
            std::push_heap(open.begin(), open.end(), heapComp);
            childNode->open = true;
        }
    }

    return TravelNodeRoute();
}

TravelNodeRoute TravelNodeMap::FindRouteNearestNodes(WorldPosition startPos, WorldPosition endPos,
                                            std::vector<WorldPosition>& startPath, Player* bot)
{
    if (nodes.empty() || !bot)
        return TravelNodeRoute();

    constexpr uint32 K = 3;
    if (nodes.size() < K)
        return TravelNodeRoute();

    // Single copy of the node list, find closest K for start and end
    std::vector<TravelNode*> nodesCopy = this->nodes;

    // nth_element is O(n) — partitions so the first K are the closest (unordered)
    std::nth_element(nodesCopy.begin(), nodesCopy.begin() + K, nodesCopy.end(),
                     [startPos](TravelNode* i, TravelNode* j) { return i->fDist(startPos) < j->fDist(startPos); });
    // Sort just the K closest
    std::sort(nodesCopy.begin(), nodesCopy.begin() + K,
              [startPos](TravelNode* i, TravelNode* j) { return i->fDist(startPos) < j->fDist(startPos); });

    // Save the K closest start nodes before reusing the vector for end nodes
    std::array<TravelNode*, K> startNodes;
    std::copy_n(nodesCopy.begin(), K, startNodes.begin());

    std::nth_element(nodesCopy.begin(), nodesCopy.begin() + K, nodesCopy.end(),
                     [endPos](TravelNode* i, TravelNode* j) { return i->fDist(endPos) < j->fDist(endPos); });
    std::sort(nodesCopy.begin(), nodesCopy.begin() + K,
              [endPos](TravelNode* i, TravelNode* j) { return i->fDist(endPos) < j->fDist(endPos); });

    std::array<TravelNode*, K> endNodes;
    std::copy_n(nodesCopy.begin(), K, endNodes.begin());

    // Cycle over the combinations of these K nodes.
    uint32 startI = 0, endI = 0;
    while (startI < K && endI < K)
    {
        TravelNode* startNode = startNodes[startI];
        TravelNode* endNode = endNodes[endI];

        WorldPosition startNodePosition = *startNode->getPosition();

        TravelNodeRoute route = GetNodeRoute(startNode, endNode, bot);

        if (!route.isEmpty())
        {
            // Check if the bot can actually walk to this start node using mmap pathfinding.
            if (startNodePosition.GetMapId() == bot->GetMapId())
            {
                PathGenerator path(bot);
                path.CalculatePath(startNodePosition.GetPositionX(), startNodePosition.GetPositionY(), startNodePosition.GetPositionZ());
                PathType type = path.GetPathType();
                bool reachable = !(type & ~(PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY));

                if (reachable)
                {
                    // Dense begin leg (cmangos getRoute parity). A bare
                    // {startPos, node} 2-point leg makes the spline bow over
                    // terrain across the tens-to-hundreds of yards to the first
                    // node (bots walking in the air). Reuse the dense incoming
                    // navmesh probe cropped to the node, else re-path densely.
                    std::vector<WorldPosition> newStartPath = startPath;
                    if (!startNodePosition.cropPathTo(newStartPath, 1.0f) || newStartPath.size() < 2)
                        newStartPath = startPos.getPathTo(startNodePosition, bot);
                    if (newStartPath.size() < 2)
                        newStartPath = {startPos, startNodePosition};
                    startPath = newStartPath;
                    return route;
                }
            }
            startI++;
        }

        // Prefer a different end-node.
        endI++;

        // Cycle to a different start-node if needed.
        if (endI > startI + 1)
        {
            startI++;
            endI = 0;
        }
    }

    return TravelNodeRoute();
}

// Route selection shared by both resolvers. Cycles the closest candidate
// nodes on both ends and only accepts a route whose begin AND tail legs
// are proven walkable — picking a single nearest node per end skips that
// proof: the begin leg may be unreachable (plan dies mid-flight) and the
// tail collapses to one straight node->destination segment the walker
// rejects, so the route re-derives forever within arrival range.
// Why the last getRoute on this thread returned empty — surfaced by the
// route-resolution debug whisper so failures are attributable in-game.
// thread_local: bots resolve on their map-update thread.
static thread_local std::string s_lastRouteFailReason;

std::string const& TravelNodeMap::GetLastRouteFailReason()
{
    return s_lastRouteFailReason;
}

TravelNodeRoute TravelNodeMap::getRoute(WorldPosition startPos, WorldPosition endPos,
    std::vector<WorldPosition>& startPath, std::vector<WorldPosition>& endPath, Unit* unit)
{
    s_lastRouteFailReason.clear();

    Player* botPlayer = unit ? unit->ToPlayer() : nullptr;

    // A bot standing on a transport can only start its route from nodes
    // belonging to that transport — shore nodes are unreachable from a
    // moving deck.
    uint32 const transportEntry = (unit && unit->GetTransport()) ? unit->GetTransport()->GetEntry() : 0;

    std::vector<TravelNode*> startNodes = getNodes(startPos, -1, transportEntry);
    std::vector<TravelNode*> endNodes = getNodes(endPos);
    if (startNodes.empty() || endNodes.empty())
    {
        s_lastRouteFailReason = startNodes.empty() ? "no nodes near start" : "no nodes near dest";
        return TravelNodeRoute();
    }

    // getNodes returns the list distance-sorted; keep the closest few.
    constexpr size_t MAX_CANDIDATE_NODES = 5;
    if (startNodes.size() > MAX_CANDIDATE_NODES)
        startNodes.resize(MAX_CANDIDATE_NODES);
    if (endNodes.size() > MAX_CANDIDATE_NODES)
        endNodes.resize(MAX_CANDIDATE_NODES);

    std::vector<TravelNode*> badStartNodes;
    uint32 pairsNoReach = 0, pairsNoAstar = 0, tailFails = 0, beginFails = 0;

    for (TravelNode* endNode : endNodes)
    {
        // Tail leg, computed once per end node and shared by every
        // start candidate.
        std::vector<WorldPosition> tailPath;

        // Consider the end node itself as the route start first: when
        // the walker can reach it directly, a single-node route (begin
        // leg to the node + tail leg from it) beats any pair that
        // detours through a second node. Vital for leaf nodes placed
        // inside caves — the node pulls the walk through the entrance
        // coil instead of routing backward to a distant neighbor and
        // in again along the stored link.
        std::vector<TravelNode*> orderedStartNodes = startNodes;
        auto selfItr = std::find(orderedStartNodes.begin(), orderedStartNodes.end(), endNode);
        if (selfItr != orderedStartNodes.end())
            std::rotate(orderedStartNodes.begin(), selfItr, selfItr + 1);

        for (TravelNode* startNode : orderedStartNodes)
        {
            if (std::find(badStartNodes.begin(), badStartNodes.end(), startNode) != badStartNodes.end())
                continue;

            WorldPosition startNodePosition = *startNode->getPosition();
            WorldPosition endNodePosition = *endNode->getPosition();
            float const maxStartDistance = startNode->isTransport() ? 20.0f : 1.0f;

            TravelNodeRoute route;
            if (startNode == endNode)
            {
                // Single-node route: no links to traverse — the plan is
                // just begin leg to the node plus tail leg from it.
                route = TravelNodeRoute({startNode});
            }
            else
            {
                if (!startNode->hasRouteTo(endNode))
                {
                    ++pairsNoReach;
                    continue;
                }

                route = GetNodeRoute(startNode, endNode, botPlayer);
                if (route.isEmpty())
                {
                    ++pairsNoAstar;
                    continue;
                }
            }

            // On a transport there is no walkable navmesh under the
            // bot, so the legs can't be walk-validated — return the
            // route with empty legs; the transport leg carries the bot
            // to solid ground, where later legs walk normally.
            if (transportEntry)
            {
                startPath.clear();
                endPath.clear();
                return route;
            }

            if (tailPath.empty())
            {
                if (startPos.GetMapId() == endPos.GetMapId())
                {
                    // Same-map travel: the tail must be a live navmesh
                    // path from the end node to the destination that
                    // actually arrives; otherwise this end node is
                    // unusable — move on to the next one.
                    tailPath = endNodePosition.getPathTo(endPos, unit);
                    bool hasEndPath = endPos.isPathTo(tailPath, 1.0f);

                    if (!hasEndPath)
                    {
                        // Underwater legs often fail on the sea floor
                        // but succeed from the surface — retry there.
                        WorldPosition surfaceNode = endNodePosition;
                        WorldPosition surfaceEnd = endPos;
                        if (surfaceNode.setAtWaterSurface() || surfaceEnd.setAtWaterSurface())
                        {
                            tailPath = surfaceNode.getPathTo(surfaceEnd, unit);
                            hasEndPath = surfaceEnd.isPathTo(tailPath, 1.0f);
                        }
                    }

                    if (!hasEndPath)
                    {
                        ++tailFails;
                        tailPath.clear();
                        break;
                    }
                }
                else
                {
                    // Cross-map: the mover's map changes mid-route, so a
                    // live tail can't be computed from here; the route is
                    // re-resolved after the transition.
                    tailPath = {endNodePosition, endPos};
                }
            }

            // Begin leg: crop the caller's probe to the start node when
            // it already passes close enough; otherwise path live. A
            // start node neither reaches is skipped for every end node.
            std::vector<WorldPosition> newStartPath = startPath;
            bool hasPath = startNodePosition.cropPathTo(newStartPath, maxStartDistance);
            if (!hasPath)
            {
                newStartPath = startPos.getPathTo(startNodePosition, unit);
                hasPath = startNodePosition.isPathTo(newStartPath, maxStartDistance);
            }
            if (!hasPath)
            {
                // Underwater begin legs: retry from the surface.
                WorldPosition surfaceStart = startPos;
                WorldPosition surfaceNode = startNodePosition;
                if (surfaceStart.setAtWaterSurface() || surfaceNode.setAtWaterSurface())
                {
                    newStartPath = surfaceStart.getPathTo(surfaceNode, unit);
                    hasPath = surfaceNode.isPathTo(newStartPath, maxStartDistance);
                }
            }
            if (!hasPath)
            {
                ++beginFails;
                badStartNodes.push_back(startNode);
                continue;
            }

            startPath = newStartPath;
            endPath = tailPath;
            return route;
        }
    }

    // Trust-the-nodes fallback: no end node has a validated mmap tail to
    // the destination (unreachable pocket, mesh oddity). Instead of
    // abandoning the graph — leaving the caller to follow a raw probe
    // into terrain — ride the graph to the node NEAREST the destination
    // and re-resolve from there: the per-tick resolver retries mmap from
    // much closer, where the final approach (steep-assisted legs,
    // unstick) can finish the job. Only the begin leg must validate; the
    // route ends AT the node (empty tail). Skipped once the bot is
    // already at that node — riding gains nothing then, and the caller's
    // probe/final-approach machinery takes over.
    // Only ride when the graph materially beats the caller's probe: the
    // fallback exists for "probe dead-ends far away, nodes reach much
    // closer". A node that can't end meaningfully closer than the probe
    // already gets would be backward motion or a self-node loop (a probe
    // from a node always ends closer than the node itself).
    float probeGap = 1000000.0f;
    if (!startPath.empty())
        probeGap = endPos.distance(startPath.back());

    if (startPos.GetMapId() == endPos.GetMapId() &&
        startPos.distance(*endNodes.front()->getPosition()) > 20.0f)
    {
        for (TravelNode* endNode : endNodes)  // distance-sorted: nearest to dest first
        {
            if (endPos.distance(*endNode->getPosition()) + 10.0f >= probeGap)
                continue;  // riding can't end meaningfully closer than the probe

            for (TravelNode* startNode : startNodes)
            {
                if (std::find(badStartNodes.begin(), badStartNodes.end(), startNode) != badStartNodes.end())
                    continue;

                WorldPosition startNodePosition = *startNode->getPosition();
                float const maxStartDistance = startNode->isTransport() ? 20.0f : 1.0f;

                TravelNodeRoute route;
                if (startNode == endNode)
                {
                    route = TravelNodeRoute({startNode});
                }
                else
                {
                    if (!startNode->hasRouteTo(endNode))
                        continue;
                    route = GetNodeRoute(startNode, endNode, botPlayer);
                    if (route.isEmpty())
                        continue;
                }

                std::vector<WorldPosition> newStartPath = startPath;
                bool hasPath = startNodePosition.cropPathTo(newStartPath, maxStartDistance);
                if (!hasPath)
                {
                    newStartPath = startPos.getPathTo(startNodePosition, unit);
                    hasPath = startNodePosition.isPathTo(newStartPath, maxStartDistance);
                }
                if (!hasPath)
                {
                    badStartNodes.push_back(startNode);
                    continue;
                }

                startPath = newStartPath;
                endPath.clear();  // walker stops at the node; re-resolution continues from there
                s_lastRouteFailReason = "node-approach (no mmap tail to dest)";
                return route;
            }
        }
    }

    s_lastRouteFailReason = "noReach:" + std::to_string(pairsNoReach) +
                            " noAstar:" + std::to_string(pairsNoAstar) +
                            " tailFail:" + std::to_string(tailFails) +
                            " beginFail:" + std::to_string(beginFails) +
                            " (s:" + std::to_string(startNodes.size()) +
                            " e:" + std::to_string(endNodes.size()) + ")";
    return TravelNodeRoute();
}

bool TravelNodeMap::GetFullPath(TravelPlan& plan,
    WorldPosition botPos, [[maybe_unused]] uint32 botZoneId,
    WorldPosition destination, Unit* bot)
{
    plan.Reset();
    plan.destination = destination;

    // mmap-probe first: if a 40-step probe reaches dest, skip the
    // graph entirely — a direct walk beats a node hop. When it falls
    // short, keep it: getRoute crops it into the begin leg when it
    // already passes near a start node.
    std::vector<WorldPosition> beginPath;
    if (botPos.GetMapId() == destination.GetMapId())
    {
        // Flat-only probe: with the steep fallback a probe can "reach"
        // over a ridge, short-circuiting the node graph — nodes exist to
        // route around exactly those spots.
        beginPath = destination.getPathFromPath({botPos}, bot, 40, /*allowSteepFallback=*/false);
        // Tight accept (10y, was spellDistance 28.5y): a probe that stops
        // 20+y short — behind a wall inside a tree trunk, on the wrong
        // side of a fence — must NOT count as "reached" and skip the node
        // graph; a genuinely complete path ends within a yard anyway.
        if (beginPath.size() >= 2 && destination.isPathTo(beginPath, 10.0f))
        {
            plan.steps.addPoint(botPos, PathNodeType::NODE_PREPATH);
            for (size_t i = 1; i < beginPath.size(); ++i)
                plan.steps.addPoint(beginPath[i], PathNodeType::NODE_PATH);
            return true;
        }
    }

    std::shared_lock<std::shared_timed_mutex> guard(m_nMapMtx);

    std::vector<WorldPosition> endPath;
    TravelNodeRoute route = getRoute(botPos, destination, beginPath, endPath, bot);
    if (route.isEmpty())
        return false;

    // BuildPath gets no unit on purpose: with one it rebuilds missing
    // link paths at runtime, mutating the shared node graph under only
    // the shared_lock held here — a data race across map-update
    // threads. Without a unit an incomplete link falls back to a plain
    // move-to-node point, which the walker handles.
    plan.steps = route.BuildPath(beginPath, endPath, nullptr);

    return !plan.steps.empty();
}

// Ported from cmangos TravelNode.cpp:1894. Resolve a full A->B TravelPath: try a
// 40-step mmap probe first (direct walk beats a node hop), otherwise route
// through the node graph and assemble with buildPath. The cmangos
// MovementActions dispatch (Phase B) calls this. Coexists with GetFullPath above
// (modpb's TravelPlan-based resolver), removed in Phase C.
TravelPath TravelNodeMap::getFullPath(WorldPosition startPos, WorldPosition endPos, Unit* unit)
{
    TravelPath movePath;
    std::vector<WorldPosition> beginPath, endPath;

    // Flat-only probe: with the steep fallback a probe can "reach" over a
    // ridge, short-circuiting the node graph — nodes exist to route around
    // exactly those spots. Steep assistance stays available to getRoute's
    // begin/tail legs and short final approaches via getPathTo.
    beginPath = endPos.getPathFromPath({startPos}, unit, 40, /*allowSteepFallback=*/false);

    // Tight accept (10y, was spellDistance 28.5y): a probe that stops 20+y
    // short — behind a wall inside a tree trunk — must NOT count as
    // "reached" and skip the node graph; a complete path ends within a
    // yard anyway.
    bool reachedByNavmesh = endPos.isPathTo(beginPath, 10.0f);

    if (reachedByNavmesh)  // If we can get within spell distance a longer route won't help.
    {
        // Disambiguate the whisper: nodes were deliberately skipped
        // because the raw probe already reaches within spellDistance.
        // If a bot still walks into terrain here, the probe FALSELY
        // reports "reached" (its endpoint is within spellDistance of
        // the target but on the wrong side of the obstacle) — a
        // different diagnosis than a failed node route.
        float const probeGap = beginPath.empty() ? -1.0f : endPos.distance(beginPath.back());
        s_lastRouteFailReason = "navmesh-reached spellDist (nodes skipped), probeGap=" +
                                std::to_string((int)probeGap) + " pts=" + std::to_string(beginPath.size());
        return TravelPath(beginPath);
    }

    // [[Node pathfinding system]]
    // Find nodes near the bot and near the end position that have a route between them, then move
    // towards/along the route.
    sTravelNodeMap.m_nMapMtx.lock_shared();

    // How far the raw mmap probe reached before node routing — the key
    // datum for the no-node case (a bot stalling at a ridge foot means
    // the pure navmesh could not round the obstacle in one resolve).
    float const rawProbeGap = beginPath.empty() ? -1.0f : endPos.distance(beginPath.back());
    size_t const rawProbePts = beginPath.size();

    // Route selection with validated dense begin AND tail legs (getRoute
    // parity with the reference): candidate node cycling, transport-aware
    // starts, single-node leaf routes and water-surface retries all live
    // in getRoute; the probe computed above feeds its begin-leg crop.
    TravelNodeRoute route = sTravelNodeMap.getRoute(startPos, endPos, beginPath, endPath, unit);

    if (route.isEmpty())
    {
        s_lastRouteFailReason = "rawProbeGap=" + std::to_string((int)rawProbeGap) + "/" +
                                std::to_string(rawProbePts) + "pts | " + s_lastRouteFailReason;

        // modpb's FindRouteNearestNodes creates no temp nodes, so there is nothing to clean up here
        // (cmangos calls route.cleanTempNodes(); modpb's TravelNodeRoute has no tempNodes).
        sTravelNodeMap.m_nMapMtx.unlock_shared();

        // The node graph can be fragmented (cross-map links absent, distant same-map nodes in separate
        // components), so the route is often empty for long travels. But getPathFromPath above usually
        // computed a real navmesh path partway to the destination (beginPath). Instead of discarding that
        // progress and freezing, follow the partial navmesh path: the bot walks toward its target and
        // re-paths from the new spot next tick. Falls back to empty only when there's no on-map navmesh
        // progress (cross-map, or beginPath is empty). Mirrors mod-cmangosbots' early-return.
        if (beginPath.size() > 1)
        {
            LOG_DEBUG("playerbots", "[Travel] no node route ({:.0f},{:.0f}) -> ({:.0f},{:.0f}) map {}, following partial probe ({} pts)",
                      startPos.GetPositionX(), startPos.GetPositionY(), endPos.GetPositionX(), endPos.GetPositionY(),
                      startPos.GetMapId(), uint32(beginPath.size()));
            return TravelPath(beginPath);
        }

        return movePath;
    }

    // buildPath gets no unit on purpose: with one it rebuilds missing
    // link paths at runtime, mutating the shared node graph under only
    // the shared_lock held here — a data race across map-update
    // threads. Without a unit an incomplete link degrades to a plain
    // move-to-node point, which the path walker handles.
    movePath = route.buildPath(beginPath, endPath, nullptr);

    sTravelNodeMap.m_nMapMtx.unlock_shared();

    return movePath;
}

bool TravelNodeMap::cropUselessNode(TravelNode* startNode)
{
    if (!startNode->isLinked() || startNode->isImportant())
        return false;

    std::vector<TravelNode*> ignore = {startNode};

    for (auto& node : getNodes(*startNode->getPosition(), 5000.f))
    {
        if (startNode == node)
            continue;

        if (node->getNodeMap(true).size() > node->getNodeMap(true, ignore).size())
            return false;
    }

    removeNode(startNode);

    return true;
}

TravelNode* TravelNodeMap::addZoneLinkNode(TravelNode* startNode)
{
    for (auto& path : *startNode->getPaths())
    {
        //TravelNode* endNode = path.first; //not used, line marked for removal.

        std::string zoneName = startNode->getPosition()->getAreaName(true, true);
        for (auto& pos : path.second.GetPath())
        {
            std::string const newZoneName = pos.getAreaName(true, true);
            if (zoneName != newZoneName)
            {
                if (!getNode(pos, nullptr, 100.0f))
                {
                    std::string const nodeName = zoneName + " to " + newZoneName;
                    return TravelNodeMap::instance().addNode(pos, nodeName, false, true);
                }

                zoneName = newZoneName;
            }
        }
    }

    return nullptr;
}

TravelNode* TravelNodeMap::addRandomExtNode(TravelNode* startNode)
{
    std::unordered_map<TravelNode*, TravelNodePath> paths = *startNode->getPaths();

    if (paths.empty())
        return nullptr;

    for (uint32 i = 0; i < 20; i++)
    {
        auto random_it = std::next(std::begin(paths), urand(0, paths.size() - 1));

        TravelNode* endNode = random_it->first;
        std::vector<WorldPosition> path = random_it->second.GetPath();

        if (path.empty())
            continue;

        // Prefer to skip complete links
        if (endNode->hasLinkTo(startNode) && startNode->hasLinkTo(endNode) && !urand(0, 20))
            continue;

        // Prefer to skip no links
        if (!startNode->hasLinkTo(endNode) && !urand(0, 20))
            continue;

        WorldPosition point = path[urand(0, path.size() - 1)];

        if (!getNode(point, nullptr, 100.0f))
            return TravelNodeMap::instance().addNode(point, startNode->getName(), false, true);
    }

    return nullptr;
}

void TravelNodeMap::generateNpcNodes()
{
    std::unordered_map<uint32, std::pair<CreatureTemplate const*, WorldPosition>> bossMap;

    for (auto& creatureData : WorldPosition().getCreaturesNear())
    {
        WorldPosition guidP(creatureData->mapid, creatureData->posX, creatureData->posY, creatureData->posZ,
                            creatureData->orientation);

        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creatureData->id);
        if (!cInfo)
            continue;

        uint32 flagMask = UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_SPIRITHEALER |
                          UNIT_NPC_FLAG_SPIRITGUIDE;

        if (cInfo->npcflag & flagMask)
        {
            std::string nodeName = guidP.getAreaName(false);

            if (cInfo->npcflag & UNIT_NPC_FLAG_INNKEEPER)
                nodeName += " innkeeper";
            else if (cInfo->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER)
                nodeName += " flightMaster";
            else if (cInfo->npcflag & UNIT_NPC_FLAG_SPIRITHEALER)
                nodeName += " spirithealer";
            else if (cInfo->npcflag & UNIT_NPC_FLAG_SPIRITGUIDE)
                nodeName += " spiritguide";

            /*TravelNode* node = */ TravelNodeMap::instance().addNode(guidP, nodeName, true, true); //node not used, fragment marked for removal.
        }
        else if (cInfo->rank == 3)
        {
            std::string const nodeName = cInfo->Name;

            TravelNodeMap::instance().addNode(guidP, nodeName, true, true);
        }
        else if (cInfo->rank == 1 && !guidP.isOverworld())
        {
            if (bossMap.find(cInfo->Entry) == bossMap.end())
                bossMap[cInfo->Entry] = std::make_pair(cInfo, guidP);
            else if (bossMap[cInfo->Entry].second)
                bossMap[cInfo->Entry] = std::make_pair(nullptr, GuidPosition());
        }
    }

    for (auto boss : bossMap)
    {
        WorldPosition guidP = boss.second.second;
        if (!guidP)
            continue;

        CreatureTemplate const* cInfo = boss.second.first;
        if (!cInfo)
            continue;

        std::string const nodeName = cInfo->Name;

        TravelNodeMap::instance().addNode(guidP, nodeName, true, true);
    }
}

void TravelNodeMap::generateStartNodes()
{
    std::map<uint8, std::string> startNames;
    startNames[RACE_HUMAN] = "Human";
    startNames[RACE_ORC] = "Orc and Troll";
    startNames[RACE_DWARF] = "Dwarf and Gnome";
    startNames[RACE_NIGHTELF] = "Night Elf";
    startNames[RACE_UNDEAD_PLAYER] = "Undead";
    startNames[RACE_TAUREN] = "Tauren";
    startNames[RACE_GNOME] = "Dwarf and Gnome";
    startNames[RACE_TROLL] = "Orc and Troll";

    for (uint32 i = 0; i < sRaceMgr->GetMaxRaces(); i++)
    {
        for (uint32 j = 0; j < MAX_CLASSES; j++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(i, j);

            if (!info)
                continue;

            WorldPosition pos(info->mapId, info->positionX, info->positionY, info->positionZ, info->orientation);

            std::string const nodeName = startNames[i] + " start";

            TravelNodeMap::instance().addNode(pos, nodeName, true, true);

            break;
        }
    }
}

void TravelNodeMap::generateAreaTriggerNodes()
{
    // Entrance nodes

    for (auto const& itr : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTriggerTeleport const& atEntry = itr.second;

        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(itr.first);
        if (!at)
            continue;

        WorldPosition inPos = WorldPosition(at->map, at->x, at->y, at->z, at->orientation);
        WorldPosition outPos = WorldPosition(atEntry.target_mapId, atEntry.target_X, atEntry.target_Y, atEntry.target_Z,
                                             atEntry.target_Orientation);

        std::string nodeName;

        if (!outPos.isOverworld())
            nodeName = outPos.getAreaName(false) + " entrance";
        else if (!inPos.isOverworld())
            nodeName = inPos.getAreaName(false) + " exit";
        else
            nodeName = inPos.getAreaName(false) + " portal";

        TravelNodeMap::instance().addNode(inPos, nodeName, true, true);
    }

    // Exit nodes

    for (auto const& itr : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTriggerTeleport const& atEntry = itr.second;

        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(itr.first);
        if (!at)
            continue;

        WorldPosition inPos = WorldPosition(at->map, at->x, at->y, at->z, at->orientation);
        WorldPosition outPos = WorldPosition(atEntry.target_mapId, atEntry.target_X, atEntry.target_Y, atEntry.target_Z,
                                             atEntry.target_Orientation);

        std::string nodeName;

        if (!outPos.isOverworld())
            nodeName = outPos.getAreaName(false) + " entrance";
        else if (!inPos.isOverworld())
            nodeName = inPos.getAreaName(false) + " exit";
        else
            nodeName = inPos.getAreaName(false) + " portal";

        //TravelNode* entryNode = TravelNodeMap::instance().getNode(outPos, nullptr, 20.0f);  // Entry side, portal exit. //not used, line marked for removal.

        TravelNode* outNode = TravelNodeMap::instance().addNode(outPos, nodeName, true, true);  // Exit size, portal exit.

        TravelNode* inNode = TravelNodeMap::instance().getNode(inPos, nullptr, 5.0f);  // Entry side, portal center.

        // Portal link from area trigger to area trigger destination.
        if (outNode && inNode)
        {
            TravelNodePath travelPath(0.1f, 3.0f, (uint8)TravelNodePathType::portal, itr.first, true);
            travelPath.setPath({*inNode->getPosition(), *outNode->getPosition()});
            inNode->setPathTo(outNode, travelPath);
        }
    }
}

void TravelNodeMap::generateTransportNodes()
{
    for (auto const& itr : *sObjectMgr->GetGameObjectTemplates())
    {
        GameObjectTemplate const* data = &itr.second;
        if (!data || (data->type != GAMEOBJECT_TYPE_TRANSPORT && data->type != GAMEOBJECT_TYPE_MO_TRANSPORT))
            continue;

        uint32 pathId = data->moTransport.taxiPathId;
        float moveSpeed = data->moTransport.moveSpeed;
        if (pathId >= sTaxiPathNodesByPath.size())
            continue;

        TaxiPathNodeList const& path = sTaxiPathNodesByPath[pathId];

        // Keep only transports with taxi paths (boats/zeppelins).
        if (path.empty())
            continue;

        std::vector<WorldPosition> ppath;
        TravelNode* prevNode = nullptr;

        // Loop over the path and connect stop locations.
        for (auto& p : path)
        {
            WorldPosition pos = WorldPosition(p->mapid, p->x, p->y, p->z, 0);

            if (prevNode)
                ppath.push_back(pos);

            if (p->delay > 0)
            {
                TravelNode* node = TravelNodeMap::instance().addNode(pos, data->name, true, true, true, itr.first);

                if (!prevNode)
                {
                    ppath.push_back(pos);
                }
                else
                {
                    TravelNodePath travelPath(0.1f, 0.0, (uint8)TravelNodePathType::transport, itr.first, true);
                    travelPath.setPathAndCost(ppath, moveSpeed);
                    node->setPathTo(prevNode, travelPath);
                    ppath.clear();
                    ppath.push_back(pos);
                }

                prevNode = node;
            }
        }

        if (!prevNode)
            continue;

        // Continue from start until first stop and connect to end.
        for (auto& p : path)
        {
            WorldPosition pos = WorldPosition(p->mapid, p->x, p->y, p->z, 0);
            ppath.push_back(pos);

            if (p->delay > 0)
            {
                TravelNode* node = TravelNodeMap::instance().getNode(pos, nullptr, 5.0f);

                if (node != prevNode)
                {
                    TravelNodePath travelPath(0.1f, 0.0, (uint8)TravelNodePathType::transport, itr.first, true);
                    travelPath.setPathAndCost(ppath, moveSpeed);

                    node->setPathTo(prevNode, travelPath);
                }
            }
        }
        ppath.clear();
    }
}

void TravelNodeMap::generateZoneMeanNodes()
{
    // Zone means
    for (auto& loc : TravelMgr::instance().exploreLocs)
    {
        std::vector<WorldPosition*> points;

        for (auto p : loc.second->getPoints(true))
            if (!p->isUnderWater())
                points.push_back(p);

        if (points.empty())
            points = loc.second->getPoints(true);

        WorldPosition pos = WorldPosition(points, WP_MEAN_CENTROID);

        /*TravelNode* node = */TravelNodeMap::instance().addNode(pos, pos.getAreaName(), true, true, false); //node not used, but addNode as side effect, fragment marked for removal.
    }
}

void TravelNodeMap::generateNodes()
{
    LOG_INFO("playerbots", "-Generating Start nodes");
    generateStartNodes();
    LOG_INFO("playerbots", "-Generating npc nodes");
    generateNpcNodes();
    LOG_INFO("playerbots", "-Generating area trigger nodes");
    generateAreaTriggerNodes();
    LOG_INFO("playerbots", "-Generating transport nodes");
    generateTransportNodes();
    LOG_INFO("playerbots", "-Generating zone mean nodes");
    generateZoneMeanNodes();
}

void TravelNodeMap::generateWalkPaths()
{
    // Pathfinder
    std::vector<WorldPosition> ppath;

    std::map<uint32, bool> nodeMaps;

    for (auto& startNode : TravelNodeMap::instance().getNodes())
    {
        nodeMaps[startNode->GetMapId()] = true;
    }

    for (auto& map : nodeMaps)
    {
        for (auto& startNode : TravelNodeMap::instance().getNodes(WorldPosition(map.first, 1, 1)))
        {
            if (startNode->isLinked())
                continue;

            for (auto& endNode : TravelNodeMap::instance().getNodes(*startNode->getPosition(), 2000.0f))
            {
                if (startNode == endNode)
                    continue;

                if (startNode->hasCompletePathTo(endNode))
                    continue;

                if (startNode->GetMapId() != endNode->GetMapId())
                    continue;

                startNode->BuildPath(endNode, nullptr, false);
            }

            startNode->setLinked(true);
        }
    }

    LOG_INFO("playerbots", ">> Generated paths for {} nodes.", TravelNodeMap::instance().getNodes().size());
}

void TravelNodeMap::generateTaxiPaths()
{
    for (uint32 i = 0; i < sTaxiPathStore.GetNumRows(); ++i)
    {
        TaxiPathEntry const* taxiPath = sTaxiPathStore.LookupEntry(i);

        if (!taxiPath)
            continue;

        TaxiNodesEntry const* startTaxiNode = sTaxiNodesStore.LookupEntry(taxiPath->from);

        if (!startTaxiNode)
            continue;

        TaxiNodesEntry const* endTaxiNode = sTaxiNodesStore.LookupEntry(taxiPath->to);

        if (!endTaxiNode)
            continue;

        TaxiPathNodeList const& nodes = sTaxiPathNodesByPath[taxiPath->ID];

        if (nodes.empty())
            continue;

        WorldPosition startPos(startTaxiNode->map_id, startTaxiNode->x, startTaxiNode->y, startTaxiNode->z);
        WorldPosition endPos(endTaxiNode->map_id, endTaxiNode->x, endTaxiNode->y, endTaxiNode->z);

        TravelNode* startNode = TravelNodeMap::instance().getNode(startPos, nullptr, 15.0f);
        TravelNode* endNode = TravelNodeMap::instance().getNode(endPos, nullptr, 15.0f);

        if (!startNode || !endNode)
            continue;

        std::vector<WorldPosition> ppath;

        for (auto& n : nodes)
            ppath.push_back(WorldPosition(n->mapid, n->x, n->y, n->z, 0.0));

        float totalTime = startPos.getPathLength(ppath) / (450 * 8.0f);

        TravelNodePath travelPath(0.1f, totalTime, (uint8)TravelNodePathType::flightPath, i, true);
        travelPath.setPath(ppath);

        // Preserve existing walk paths — taxi-position lookup can resolve to
        // a non-FM node (innkeeper, subzone), and overwriting its walk path
        // with a flight path makes the walkable connection disappear.
        if (startNode->hasPathTo(endNode) &&
            startNode->getPathTo(endNode)->getPathType() == TravelNodePathType::walk)
            continue;

        startNode->setPathTo(endNode, travelPath);
    }
}

void TravelNodeMap::removeLowNodes()
{
    std::vector<TravelNode*> goodNodes;
    std::vector<TravelNode*> remNodes;
    for (auto& node : TravelNodeMap::instance().getNodes())
    {
        if (!node->getPosition()->isOverworld())
            continue;

        if (std::find(goodNodes.begin(), goodNodes.end(), node) != goodNodes.end())
            continue;

        if (std::find(remNodes.begin(), remNodes.end(), node) != remNodes.end())
            continue;

        std::vector<TravelNode*> nodes = node->getNodeMap(true);

        if (nodes.size() < 5)
            remNodes.insert(remNodes.end(), nodes.begin(), nodes.end());
        else
            goodNodes.insert(goodNodes.end(), nodes.begin(), nodes.end());
    }

    for (auto& node : remNodes)
        TravelNodeMap::instance().removeNode(node);
}

void TravelNodeMap::removeUselessPaths()
{
    // Clean up node links
    for (auto& startNode : TravelNodeMap::instance().getNodes())
    {
        for (auto& path : *startNode->getPaths())
            if (path.second.getComplete() && startNode->hasLinkTo(path.first))
                ASSERT(true);
    }
    uint32 it = 0;
    while (true)
    {
        uint32 rem = 0;
        // Clean up node links
        for (auto& startNode : TravelNodeMap::instance().getNodes())
        {
            if (startNode->cropUselessLinks())
                rem++;
        }

        if (!rem)
            break;

        hasToSave = true;
        it++;

        LOG_INFO("playerbots", "Iteration {}, removed {}", it, rem);
    }
}

void TravelNodeMap::calculatePathCosts()
{
    for (auto& startNode : TravelNodeMap::instance().getNodes())
    {
        for (auto& path : *startNode->getLinks())
        {
            TravelNodePath* nodePath = path.second;

            if (path.second->getPathType() != TravelNodePathType::walk)
                continue;

            if (nodePath->getCalculated())
                continue;

            nodePath->calculateCost();
        }
    }

    LOG_INFO("playerbots", ">> Calculated pathcost for {} nodes.", TravelNodeMap::instance().getNodes().size());
}

void TravelNodeMap::generatePaths(bool fullGen)
{
    LOG_INFO("playerbots", "-Calculating walkable paths");
    generateWalkPaths();

    if (fullGen)
    {
        LOG_INFO("playerbots", "-Removing useless nodes");
        removeLowNodes();

        LOG_INFO("playerbots", "-Removing useless paths");
        removeUselessPaths();
    }

    LOG_INFO("playerbots", "-Calculating path costs");
    calculatePathCosts();
    LOG_INFO("playerbots", "-Generating taxi paths");
    generateTaxiPaths();
}

void TravelNodeMap::generateAll()
{
    generatePaths(false);
    hasToSave = true;
    saveNodeStore();

    BuildZoneIndex();
    PrecomputeReachability();
}

void TravelNodeMap::Init()
{
    InitTaxiGraph();

    LoadNodeStore();
    calcMapOffset();

    if (hasToGen || hasToFullGen)
    {
        if (hasToFullGen)
            generateNodes();

        generatePaths(hasToFullGen);
        hasToGen = false;
        hasToFullGen = false;
        saveNodeStore();
    }

    BuildZoneIndex();
    PrecomputeReachability();
}

void TravelNodeMap::printMap()
{
    if (!sPlayerbotAIConfig.HasLog("travelNodes.csv") && !sPlayerbotAIConfig.HasLog("travelPaths.csv"))
        return;

    printf("\r [Qgis] \r\x3D");
    fflush(stdout);

    sPlayerbotAIConfig.OpenLog("travelNodes.csv", "w");
    sPlayerbotAIConfig.OpenLog("travelPaths.csv", "w");

    std::vector<TravelNode*> anodes = getNodes();

    //uint32 nr = 0; //not used, line marked for removal.

    for (auto& node : anodes)
    {
        node->print(false);
    }
}

void TravelNodeMap::printNodeStore()
{
    std::string const nodeStore = "TravelNodeStore.h";

    if (!sPlayerbotAIConfig.HasLog(nodeStore))
        return;

    printf("\r [Map] \r\x3D");
    fflush(stdout);

    sPlayerbotAIConfig.OpenLog(nodeStore, "w");

    std::unordered_map<TravelNode*, uint32> saveNodes;

    std::vector<TravelNode*> anodes = getNodes();

    sPlayerbotAIConfig.Log(nodeStore, "#pragma once");
    sPlayerbotAIConfig.Log(nodeStore, "#include \"TravelMgr.h\"");
    sPlayerbotAIConfig.Log(nodeStore, "class TravelNodeStore");
    sPlayerbotAIConfig.Log(nodeStore, "    {");
    sPlayerbotAIConfig.Log(nodeStore, "    public:");
    sPlayerbotAIConfig.Log(nodeStore, "    static void loadNodes()");
    sPlayerbotAIConfig.Log(nodeStore, "    {");
    sPlayerbotAIConfig.Log(nodeStore, "        TravelNode** nodes = new TravelNode*[%zu];", anodes.size());

    for (uint32 i = 0; i < anodes.size(); i++)
    {
        TravelNode* node = anodes[i];

        std::ostringstream out;

        std::string name = node->getName();
        name.erase(remove(name.begin(), name.end(), '\"'), name.end());

        //        struct addNode {uint32 node; WorldPosition point; std::string const name; bool isPortal; bool
        //        isTransport; uint32 transportId; };
        out << std::fixed << std::setprecision(2) << "        addNodes.push_back(addNode{" << i << ",";
        out << "WorldPosition(" << node->GetMapId() << ", " << node->getX() << "f, " << node->getY() << "f, "
            << node->getZ() << "f, " << node->getO() << "f),";
        out << "\"" << name << "\"";
        if (node->isTransport())
            out << "," << (node->isTransport() ? "true" : "false") << "," << node->getTransportId();
        out << "});";

        /*
                out << std::fixed << std::setprecision(2) << "        nodes[" << i << "] =
           TravelNodeMap::instance().addNode(&WorldPosition(" << node->GetMapId() << "," << node->getX() << "f," << node->getY()
           << "f," << node->getZ() << "f,"<< node->getO() <<"f), \""
                    << name << "\", " << (node->isImportant() ? "true" : "false") << ", true";
                if (node->isTransport())
                    out << "," << (node->isTransport() ? "true" : "false") << "," << node->getTransportId();

                out << ");";
                */
        sPlayerbotAIConfig.Log(nodeStore, out.str().c_str());

        saveNodes.insert(std::make_pair(node, i));
    }

    for (uint32 i = 0; i < anodes.size(); i++)
    {
        TravelNode* node = anodes[i];

        for (auto& Link : *node->getLinks())
        {
            std::ostringstream out;

            //        struct linkNode { uint32 node1; uint32 node2; float distance; float extraCost; bool isPortal; bool
            //        isTransport; uint32 maxLevelMob; uint32 maxLevelAlliance; uint32 maxLevelHorde; float
            //        swimDistance; };

            out << std::fixed << std::setprecision(2) << "        linkNodes3.push_back(linkNode3{" << i << ","
                << saveNodes.find(Link.first)->second << ",";
            out << Link.second->print() << "});";

            // out << std::fixed << std::setprecision(1) << "        nodes[" << i << "]->setPathTo(nodes[" <<
            // saveNodes.find(Link.first)->second << "],TravelNodePath("; out << Link.second->print() << "), true);";
            sPlayerbotAIConfig.Log(nodeStore, out.str().c_str());
        }
    }

    sPlayerbotAIConfig.Log(nodeStore, "    }");
    sPlayerbotAIConfig.Log(nodeStore, "};");

    printf("\r [Done] \r\x3D");
    fflush(stdout);
}

void TravelNodeMap::saveNodeStore()
{
    if (!hasToSave)
        return;

    hasToSave = false;

    constexpr uint32 STMTS_PER_TX = 500;  // bounded transaction size

    // Phase 1: deletes in their own transaction.
    {
        PlayerbotsDatabaseTransaction delTrans = PlayerbotsDatabase.BeginTransaction();
        delTrans->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_TRAVELNODE));
        delTrans->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_TRAVELNODE_LINK));
        delTrans->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_TRAVELNODE_PATH));
        PlayerbotsDatabase.CommitTransaction(delTrans);
    }

    std::unordered_map<TravelNode*, uint32> saveNodes;
    std::vector<TravelNode*> anodes = TravelNodeMap::instance().getNodes();

    // Phase 2: node inserts, chunked at STMTS_PER_TX per transaction.
    {
        PlayerbotsDatabaseTransaction nodeTrans = PlayerbotsDatabase.BeginTransaction();
        uint32 inTx = 0;
        for (uint32 i = 0; i < anodes.size(); i++)
        {
            TravelNode* node = anodes[i];

            std::string name = node->getName();
            name.erase(remove(name.begin(), name.end(), '\''), name.end());

            PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_TRAVELNODE);
            stmt->SetData(0, i);
            stmt->SetData(1, name);
            stmt->SetData(2, node->GetMapId());
            stmt->SetData(3, node->getX());
            stmt->SetData(4, node->getY());
            stmt->SetData(5, node->getZ());
            stmt->SetData(6, node->isLinked());
            nodeTrans->Append(stmt);

            saveNodes.insert(std::make_pair(node, i));

            if (++inTx >= STMTS_PER_TX)
            {
                PlayerbotsDatabase.CommitTransaction(nodeTrans);
                nodeTrans = PlayerbotsDatabase.BeginTransaction();
                inTx = 0;
            }
        }
        PlayerbotsDatabase.CommitTransaction(nodeTrans);
    }

    LOG_INFO("playerbots", ">> Saved {} travelNodes.", anodes.size());

    // Phase 3: link inserts, chunked at STMTS_PER_TX per transaction.
    uint32 paths = 0;
    {
        PlayerbotsDatabaseTransaction linkTrans = PlayerbotsDatabase.BeginTransaction();
        uint32 inTx = 0;
        for (uint32 i = 0; i < anodes.size(); i++)
        {
            TravelNode* node = anodes[i];

            for (auto& link : *node->getLinks())
            {
                TravelNodePath* path = link.second;

                PlayerbotsDatabasePreparedStatement* stmt =
                    PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_TRAVELNODE_LINK);
                stmt->SetData(0, i);
                stmt->SetData(1, saveNodes.find(link.first)->second);
                stmt->SetData(2, static_cast<uint8>(path->getPathType()));
                stmt->SetData(3, path->getPathObject());
                stmt->SetData(4, path->getDistance());
                stmt->SetData(5, path->getSwimDistance());
                stmt->SetData(6, path->getExtraCost());
                stmt->SetData(7, path->getCalculated());
                stmt->SetData(8, path->getMaxLevelCreature()[0]);
                stmt->SetData(9, path->getMaxLevelCreature()[1]);
                stmt->SetData(10, path->getMaxLevelCreature()[2]);
                linkTrans->Append(stmt);

                paths++;

                if (++inTx >= STMTS_PER_TX)
                {
                    PlayerbotsDatabase.CommitTransaction(linkTrans);
                    linkTrans = PlayerbotsDatabase.BeginTransaction();
                    inTx = 0;
                }
            }
        }
        PlayerbotsDatabase.CommitTransaction(linkTrans);
    }

    // Phase 2: path points in chunked transactions. Previously all
    // ~1.5M point inserts went into a single mega-transaction which
    // exceeded MySQL's packet/transaction limits and partial-committed,
    // corrupting the DB (links saved, paths empty). Chunk now commits
    // every ~10000 rows. A failed chunk loses only its rows; the rest
    // survive.
    constexpr uint32 BATCH_SIZE = 500;
    constexpr uint32 BATCHES_PER_COMMIT = 20;  // 20 * 500 = 10000 rows per tx
    uint32 points = 0;
    std::ostringstream ss;
    uint32 batchCount = 0;
    uint32 batchesInCurrentTx = 0;
    PlayerbotsDatabaseTransaction pathTrans = PlayerbotsDatabase.BeginTransaction();

    auto flushBatch = [&]()
    {
        if (batchCount == 0)
            return;

        std::string sql = ss.str();
        sql.back() = ';';  // Replace trailing comma
        pathTrans->Append(sql.c_str());
        ss.str("");
        ss.clear();
        batchCount = 0;
        batchesInCurrentTx++;
    };

    auto commitIfFull = [&]()
    {
        if (batchesInCurrentTx >= BATCHES_PER_COMMIT)
        {
            PlayerbotsDatabase.CommitTransaction(pathTrans);
            pathTrans = PlayerbotsDatabase.BeginTransaction();
            batchesInCurrentTx = 0;
        }
    };

    for (uint32 i = 0; i < anodes.size(); i++)
    {
        TravelNode* node = anodes[i];

        for (auto& link : *node->getLinks())
        {
            TravelNodePath* path = link.second;
            uint32 toId = saveNodes.find(link.first)->second;
            std::vector<WorldPosition> ppath = path->GetPath();

            for (uint32 j = 0; j < ppath.size(); j++)
            {
                WorldPosition& point = ppath[j];

                if (batchCount == 0)
                    ss << "INSERT INTO `playerbots_travelnode_path` (`node_id`,`to_node_id`,`nr`,`map_id`,`x`,`y`,`z`) VALUES ";

                ss << std::fixed << std::setprecision(4)
                   << "(" << i << "," << toId << "," << j << ","
                   << point.GetMapId() << ","
                   << point.GetPositionX() << ","
                   << point.GetPositionY() << ","
                   << point.GetPositionZ() << "),";

                batchCount++;
                points++;

                if (batchCount >= BATCH_SIZE)
                {
                    flushBatch();
                    commitIfFull();
                }
            }
        }
    }

    flushBatch();
    PlayerbotsDatabase.CommitTransaction(pathTrans);

    LOG_INFO("playerbots", ">> Saved {} travelNode Paths, {} points.", paths, points);
    LOG_INFO("playerbots",
             ">> NOTE: writes are queued ASYNC. Run '.server shutdown 1' to flush "
             "the queue; killing the process now will lose pending rows.");
}

void TravelNodeMap::LoadNodeStore()
{
    std::string const query = "SELECT id, name, map_id, x, y, z, linked FROM playerbots_travelnode";

    std::unordered_map<uint32, TravelNode*> saveNodes;

    {
        if (PreparedQueryResult result =
                PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TRAVELNODE)))
        {
            do
            {
                Field* fields = result->Fetch();

                TravelNode* node = addNode(WorldPosition(fields[2].Get<uint32>(), fields[3].Get<float>(),
                                                         fields[4].Get<float>(), fields[5].Get<float>()),
                                           fields[1].Get<std::string>(), true, false);

                if (fields[6].Get<bool>())
                    node->setLinked(true);
                else
                    hasToGen = true;

                saveNodes.insert(std::make_pair(fields[0].Get<uint32>(), node));

            } while (result->NextRow());

            LOG_INFO("playerbots", ">> Loaded {} travelNodes.", saveNodes.size());
        }
        else
        {
            hasToFullGen = true;
            LOG_ERROR("playerbots", ">> Error loading travelNodes.");
        }
    }

    {
        if (PreparedQueryResult result =
                PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TRAVELNODE_LINK)))
        {
            do
            {
                Field* fields = result->Fetch();

                auto startIt = saveNodes.find(fields[0].Get<uint32>());
                auto endIt = saveNodes.find(fields[1].Get<uint32>());

                if (startIt == saveNodes.end() || endIt == saveNodes.end())
                    continue;

                TravelNode* startNode = startIt->second;
                TravelNode* endNode = endIt->second;

                startNode->setPathTo(
                    endNode,
                    TravelNodePath(fields[4].Get<float>(), fields[6].Get<float>(), fields[2].Get<uint8>(),
                                   fields[3].Get<uint64>(), fields[7].Get<bool>(),
                                   {fields[8].Get<uint8>(), fields[9].Get<uint8>(), fields[10].Get<uint8>()},
                                   fields[5].Get<float>()),
                    true);

                if (!fields[7].Get<bool>())
                    hasToGen = true;

            } while (result->NextRow());

            LOG_INFO("playerbots", ">> Loaded {} travelNode paths.", result->GetRowCount());
        }
        else
        {
            LOG_ERROR("playerbots", ">> Error loading travelNode links.");
        }
    }

    {
        if (PreparedQueryResult result =
                PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TRAVELNODE_PATH)))
        {
            do
            {
                Field* fields = result->Fetch();

                auto startIt = saveNodes.find(fields[0].Get<uint32>());
                auto endIt = saveNodes.find(fields[1].Get<uint32>());

                if (startIt == saveNodes.end() || endIt == saveNodes.end())
                    continue;

                TravelNode* startNode = startIt->second;
                TravelNode* endNode = endIt->second;

                if (!startNode->hasPathTo(endNode))
                    continue;

                TravelNodePath* path = startNode->getPathTo(endNode);

                std::vector<WorldPosition> ppath = path->GetPath();
                ppath.push_back(WorldPosition(fields[3].Get<uint32>(), fields[4].Get<float>(), fields[5].Get<float>(),
                                              fields[6].Get<float>()));

                path->setPath(ppath);

                if (path->getCalculated())
                    path->setComplete(true);

            } while (result->NextRow());

            LOG_INFO("playerbots", ">> Loaded {} travelNode paths points.", result->GetRowCount());
        }
        else
        {
            LOG_ERROR("playerbots", ">> Error loading travelNode paths.");
        }
    }
}

void TravelNodeMap::calcMapOffset()
{
    mapOffsets.push_back(std::make_pair(0, WorldPosition(0, 0, 0, 0, 0)));
    mapOffsets.push_back(std::make_pair(1, WorldPosition(1, -3680.0, 13670.0, 0, 0)));
    mapOffsets.push_back(std::make_pair(530, WorldPosition(530, 15000.0, -20000.0, 0, 0)));
    mapOffsets.push_back(std::make_pair(571, WorldPosition(571, 10000.0, 5000.0, 0, 0)));

    std::vector<uint32> mapIds;

    for (auto& node : nodes)
    {
        if (!node->getPosition()->isOverworld())
            if (std::find(mapIds.begin(), mapIds.end(), node->GetMapId()) == mapIds.end())
                mapIds.push_back(node->GetMapId());
    }

    std::sort(mapIds.begin(), mapIds.end());

    std::vector<WorldPosition> min, max;

    for (auto& mapId : mapIds)
    {
        bool doPush = true;
        for (auto& node : nodes)
        {
            if (node->GetMapId() != mapId)
                continue;

            if (doPush)
            {
                min.push_back(*node->getPosition());
                max.push_back(*node->getPosition());
                doPush = false;
            }
            else
            {
                min.back().setX(std::min(min.back().GetPositionX(), node->getX()));
                min.back().setY(std::min(min.back().GetPositionY(), node->getY()));
                max.back().setX(std::max(max.back().GetPositionX(), node->getX()));
                max.back().setY(std::max(max.back().GetPositionY(), node->getY()));
            }
        }
    }

    WorldPosition curPos = WorldPosition(0, -13000, -13000, 0, 0);
    WorldPosition endPos = WorldPosition(0, 3000, -13000, 0, 0);

    uint32 i = 0;
    float maxY = 0;
    //+X -> -Y
    for (auto& mapId : mapIds)
    {
        mapOffsets.push_back(std::make_pair(
            mapId, WorldPosition(mapId, curPos.GetPositionX() - min[i].GetPositionX(),
                                 curPos.GetPositionY() - max[i].GetPositionY(), 0, 0)));

        maxY = std::max(maxY, (max[i].GetPositionY() - min[i].GetPositionY() + 500));
        curPos.setX(curPos.GetPositionX() + (max[i].GetPositionX() - min[i].GetPositionX() + 500));

        if (curPos.GetPositionX() > endPos.GetPositionX())
        {
            curPos.setY(curPos.GetPositionY() - maxY);
            curPos.setX(-13000);
        }

        i++;
    }
}

WorldPosition TravelNodeMap::getMapOffset(uint32 mapId)
{
    for (auto& offset : mapOffsets)
    {
        if (offset.first == mapId)
            return offset.second;
    }

    return WorldPosition(mapId, 0, 0, 0, 0);
}

// TravelNodeMap taxi graph (BFS-based flight path lookup)
void TravelNodeMap::InitTaxiGraph()
{
    BuildTaxiGraph();
    ComputeAllPaths();
}

std::vector<uint32> TravelNodeMap::FindTaxiPath(uint32 fromNode, uint32 toNode)
{
    if (fromNode == toNode)
        return {};

    TaxiNodesEntry const* startNode = sTaxiNodesStore.LookupEntry(fromNode);
    TaxiNodesEntry const* endNode = sTaxiNodesStore.LookupEntry(toNode);

    if (!startNode || !endNode)
        return {};

    auto cacheItr = m_taxiPathCache.find(fromNode);
    if (cacheItr == m_taxiPathCache.end())
        return {};

    auto toNodeItr = cacheItr->second.find(toNode);
    if (toNodeItr == cacheItr->second.end())
        return {};

    return toNodeItr->second;
}

void TravelNodeMap::BuildTaxiGraph()
{
    m_taxiGraph.clear();
    std::unordered_map<uint32, std::unordered_set<uint32>> tempGraph;
    for (uint32 i = 0; i < sTaxiPathStore.GetNumRows(); ++i)
    {
        TaxiPathEntry const* path = sTaxiPathStore.LookupEntry(i);
        if (!path)
            continue;

        if (path->to == 0 || path->to == uint32(-1))
            continue;

        tempGraph[path->from].insert(path->to);
    }
    for (auto const& [node, neighbors] : tempGraph)
        m_taxiGraph[node] = std::vector<uint32>(neighbors.begin(), neighbors.end());
}

void TravelNodeMap::ComputeAllPaths()
{
    std::set<uint32> allNodes;
    for (auto const& [source, neighbors] : m_taxiGraph)
    {
        allNodes.insert(source);
        allNodes.insert(neighbors.begin(), neighbors.end());
    }

    for (uint32 source : allNodes)
    {
        auto parentMap = BFS(source);

        for (uint32 target : allNodes)
        {
            if (source == target)
                continue;

            auto path = BuildPath(source, target, parentMap);
            if (!path.empty())
                m_taxiPathCache[source][target] = path;
        }
    }
}

std::unordered_map<uint32, uint32> TravelNodeMap::BFS(uint32 fromNode)
{
    std::queue<uint32> workQueue;
    std::unordered_set<uint32> visited;
    std::unordered_map<uint32, uint32> parentMap;

    workQueue.push(fromNode);
    visited.insert(fromNode);
    parentMap[fromNode] = 0;

    while (!workQueue.empty())
    {
        uint32 current = workQueue.front();
        workQueue.pop();

        auto graphItr = m_taxiGraph.find(current);
        if (graphItr == m_taxiGraph.end())
            continue;

        for (uint32 next : graphItr->second)
        {
            if (visited.count(next))
                continue;

            visited.insert(next);
            parentMap[next] = current;
            workQueue.push(next);
        }
    }
    return parentMap;
}

std::vector<uint32> TravelNodeMap::BuildPath(uint32 fromNode, uint32 toNode,
                                              std::unordered_map<uint32, uint32> const& parentMap)
{
    if (!parentMap.count(toNode))
        return {}; // unreachable

    std::vector<uint32> path;
    uint32 current = toNode;
    while (current != fromNode)
    {
        path.push_back(current);
        auto it = parentMap.find(current);
        if (it == parentMap.end() || it->second == 0)
            break;
        current = it->second;
    }

    path.push_back(fromNode);
    std::reverse(path.begin(), path.end());
    return path;
}

void TravelNodeMap::BuildZoneIndex()
{
    m_zoneIndex.clear();
    m_mapIndex.clear();

    for (auto* node : nodes)
    {
        if (!node)
            continue;

        WorldPosition* pos = node->getPosition();
        uint32 mapId = pos->GetMapId();

        m_mapIndex[mapId].push_back(node);

        uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, *pos);
        if (zoneId)
            m_zoneIndex[zoneId].push_back(node);
    }
}

TravelNode* TravelNodeMap::GetNearestNodeInZone(WorldPosition pos, uint32 zoneId)
{
    auto it = m_zoneIndex.find(zoneId);
    if (it == m_zoneIndex.end() || it->second.empty())
        return GetNearestNodeOnMap(pos);  // Fallback to map-wide

    TravelNode* bestNode = nullptr;
    float bestDist = FLT_MAX;

    for (auto* node : it->second)
    {
        if (!node || node->GetMapId() != pos.GetMapId())
            continue;
        float dist = node->fDist(pos);
        if (dist < bestDist)
        {
            bestDist = dist;
            bestNode = node;
        }
    }

    if (!bestNode)
        return GetNearestNodeOnMap(pos);

    return bestNode;
}

std::vector<TravelNode*> const& TravelNodeMap::GetNodesInZone(uint32 zoneId) const
{
    static std::vector<TravelNode*> const empty;
    auto it = m_zoneIndex.find(zoneId);
    if (it == m_zoneIndex.end())
        return empty;
    return it->second;
}

TravelNode* TravelNodeMap::GetNearestNodeOnMap(WorldPosition pos)
{
    auto it = m_mapIndex.find(pos.GetMapId());
    if (it == m_mapIndex.end() || it->second.empty())
        return nullptr;

    TravelNode* bestNode = nullptr;
    float bestDist = FLT_MAX;

    for (auto* node : it->second)
    {
        if (!node)
            continue;
        float d = node->fDist(pos);
        if (d < bestDist)
        {
            bestDist = d;
            bestNode = node;
        }
    }

    return bestNode;
}

void TravelNodeMap::PrecomputeReachability()
{
    // Links are one-directional, so a BFS over outgoing links with one
    // global visited set is order-dependent: a node whose only edge
    // into the already-visited part of the graph is one-way gets
    // stranded in a singleton component, and hasRouteTo then rejects
    // genuinely routable pairs — route selection falls back to raw
    // probes that walk into terrain the graph would route around.
    // Compute components over the UNDIRECTED edge closure instead:
    // false negatives disappear; the occasional false positive (a
    // one-way-unreachable pair marked routable) is harmless because
    // the A* itself is the ground truth and returns empty for it.
    std::unordered_map<TravelNode*, std::vector<TravelNode*>> reverseLinks;
    for (auto* node : nodes)
    {
        if (!node)
            continue;
        for (auto const& link : *node->getLinks())
            if (link.first)
                reverseLinks[link.first].push_back(node);
    }

    std::unordered_set<TravelNode*> visited;
    std::vector<std::vector<TravelNode*>> components;

    for (auto* node : nodes)
    {
        if (!node || visited.count(node))
            continue;

        // BFS from this node, expanding along both edge directions.
        std::vector<TravelNode*> component;
        std::queue<TravelNode*> q;
        q.push(node);
        visited.insert(node);

        while (!q.empty())
        {
            TravelNode* current = q.front();
            q.pop();
            component.push_back(current);

            for (auto const& link : *current->getLinks())
            {
                TravelNode* neighbor = link.first;
                if (neighbor && !visited.count(neighbor))
                {
                    visited.insert(neighbor);
                    q.push(neighbor);
                }
            }

            auto revItr = reverseLinks.find(current);
            if (revItr != reverseLinks.end())
            {
                for (TravelNode* neighbor : revItr->second)
                {
                    if (neighbor && !visited.count(neighbor))
                    {
                        visited.insert(neighbor);
                        q.push(neighbor);
                    }
                }
            }
        }

        components.push_back(std::move(component));
    }

    // Populate routes: every node in a component can reach every other node
    // in the same component
    for (auto const& comp : components)
    {
        for (auto* node : comp)
        {
            node->clearRoutes();
            for (auto* other : comp)
                node->setRouteTo(other);
        }
    }
}
