#include "CreatureCore.h"
#include "CreaturePluginPCH.h"
#include "CreatureMetaAsset.h"
#include "Engine/Engine.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFilemanager.h"

DECLARE_CYCLE_STAT(TEXT("CreatureCore_RunTick"), STAT_CreatureCore_RunTick, STATGROUP_Creature);
DECLARE_CYCLE_STAT(TEXT("CreatureCore_UpdateCreatureRender"), STAT_CreatureCore_UpdateCreatureRender, STATGROUP_Creature);
DECLARE_CYCLE_STAT(TEXT("CreatureCore_FillBoneData"), STAT_CreatureCore_FillBoneData, STATGROUP_Creature);
DECLARE_CYCLE_STAT(TEXT("CreatureCore_ParseEvents"), STAT_CreatureCore_ParseEvents, STATGROUP_Creature);
DECLARE_CYCLE_STAT(TEXT("CreatureCore_UpdateManager"), STAT_CreatureCore_UpdateManager, STATGROUP_Creature);
DECLARE_CYCLE_STAT(TEXT("CreatureCore_SetActiveAnimation"), STAT_CreatureCore_SetActiveAnimation, STATGROUP_Creature);

static TMap<FName, TSharedPtr<CreatureModule::CreatureAnimation> > global_animations;
static TMap<FName, TSharedPtr<CreatureModule::CreatureLoadDataPacket> > global_load_data_packets;

// Misc Functions
static FName GetAnimationToken(const FName& filename_in, const FName& name_in)
{
	return FName(*FString::Printf(TEXT("%s_%s"), *filename_in.ToString(), *name_in.ToString()));
}

std::string ConvertToString(const FString &str)
{
	std::string t = TCHAR_TO_UTF8(*str);
	return t;
}
std::string ConvertToString(FName name)
{
	std::string t = TCHAR_TO_UTF8(*name.ToString());
	return t;
}

// CreatureMeshDataModifier
CreatureMeshDataModifier::CreatureMeshDataModifier(int32 num_indices, int32 num_pts)
{
	m_indices.SetNum(num_indices);
	m_pts.SetNum(num_pts * 3);
	m_uvs.SetNum(num_pts * 2);
	m_colors.SetNum(num_pts);
}

void CreatureMeshDataModifier::initData(CreatureCore& core_in)
{
	if (m_initCB)
	{
		m_initCB(*this, core_in);
	}
}

void CreatureMeshDataModifier::update(CreatureCore& core_in)
{
	if (m_updateCB)
	{
		m_updateCB(*this, core_in);
	}
}

int CreatureMeshDataModifier::numPoints() const
{
	return m_pts.Num() / 3;
}

// CreatureCore
CreatureCore::CreatureCore()
{
	pJsonData = nullptr;
	smooth_transitions = false;
	bone_data_size = 0.01f;
	bone_data_length_factor = 0.02f;
	should_play = true;
	region_overlap_z_delta = 0.01f;
	is_looping = true;
	play_start_done = false;
	play_end_done = false;
	is_disabled = false;
	is_driven = false;
	is_ready_play = false;
	is_animation_loaded = false;
	do_file_warning = true;
	should_process_animation_start = false;
	should_process_animation_end = false;
	should_update_render_indices = false;
	meta_data = nullptr;
	global_indices_copy = nullptr;
	skin_swap_active = false;
	region_order_indices_num = 0;
	run_morph_targets = false;
	update_lock = TSharedPtr<FCriticalSection, ESPMode::ThreadSafe>(new FCriticalSection());
}

CreatureCore::~CreatureCore()
{
	ClearMemory();
}

void 
CreatureCore::ClearMemory()
{
	if (global_indices_copy)
	{
		delete[] global_indices_copy;
		global_indices_copy = nullptr;
	}
}

bool 
CreatureCore::GetAndClearShouldAnimStart()
{
	bool retval = should_process_animation_start;
	should_process_animation_start = false;

	return retval;
}

bool 
CreatureCore::GetAndClearShouldAnimEnd()
{
	bool retval = should_process_animation_end;
	should_process_animation_end = false;

	return retval;
}

FProceduralMeshTriData 
CreatureCore::GetProcMeshData(EWorldType::Type world_type)
{
	auto cur_creature = creature_manager->GetCreature();
	if ((is_animation_loaded == false) || (cur_creature == nullptr))
	{
		FProceduralMeshTriData ret_data(nullptr,
			nullptr, nullptr,
			0, 0,
			&region_colors,
			update_lock);

		return ret_data;
	}

	int32 num_points = cur_creature->GetTotalNumPoints();
	int32 num_indices = cur_creature->GetTotalNumIndices();
	glm::uint32 * cur_indices = cur_creature->GetGlobalIndices();
	glm::float32 * cur_pts = cur_creature->GetRenderPts();
	glm::float32 * cur_uvs = cur_creature->GetGlobalUvs();

	glm::uint32 * copy_indices = GetIndicesCopy(num_indices);
	std::memcpy(copy_indices, cur_indices, sizeof(glm::uint32) * num_indices);

	if (region_colors.Num() != num_points)
	{
		region_colors.SetNum(num_points);
	}

	if ((world_type == EWorldType::Type::Editor) || (world_type == EWorldType::Type::EditorPreview))
	{
		for (auto i = 0; i < region_colors.Num(); i++)
		{
			region_colors[i] = FColor(255, 255, 255, 255);
		}
	}
	
	// Determine actual points, uvs and indices to set for the render mesh
	glm::uint32 * actual_indices = copy_indices;
	glm::float32 * actual_pts = cur_pts;
	glm::float32 * actual_uvs = cur_uvs;
	int32 actual_num_points = num_points;
	int32 actual_num_indices = num_indices;
	TArray<FColor> * actual_region_colors = &region_colors;

	if (mesh_modifier.IsValid())
	{
		// Use mesh modifier
		mesh_modifier->initData(*this);
		actual_indices = mesh_modifier->m_indices.GetData();
		actual_pts = mesh_modifier->m_pts.GetData();
		actual_uvs = mesh_modifier->m_uvs.GetData();
		actual_num_points = mesh_modifier->numPoints();
		actual_num_indices = mesh_modifier->m_maxIndice;
		actual_region_colors = &(mesh_modifier->m_colors);
	}

	FProceduralMeshTriData ret_data(
		actual_indices,
		actual_pts, 
		actual_uvs,
		actual_num_points, 
		actual_num_indices,
		actual_region_colors,
		update_lock);

	return ret_data;
}

void CreatureCore::UpdateCreatureRender()
{
	SCOPE_CYCLE_COUNTER(STAT_CreatureCore_UpdateCreatureRender);

	auto cur_creature = creature_manager->GetCreature();
	int num_triangles = cur_creature->GetTotalNumIndices() / 3;
	glm::uint32 * cur_idx = cur_creature->GetGlobalIndices();
	auto cur_num_indices = cur_creature->GetTotalNumIndices();
	glm::float32 * cur_pts = cur_creature->GetRenderPts();
	glm::float32 * cur_uvs = cur_creature->GetGlobalUvs();
	should_update_render_indices = false;
	region_order_indices_num = 0;

	// Update depth per region
	TArray<meshRenderRegion *>& cur_regions =
		cur_creature->GetRenderComposition()->getRegions();
	float region_z = 0.0f, delta_z = region_overlap_z_delta;

	if (region_custom_order.Num() != cur_regions.Num())
	{
		// Normal update in default order
		for (auto& single_region : cur_regions)
		{
			glm::float32 * region_pts = cur_pts + (single_region->getStartPtIndex() * 3);
			for (int32 i = 0; i < single_region->getNumPts(); i++)
			{
				region_pts[2] = region_z;
				region_pts += 3;
			}

			region_z += delta_z;
		}

		// Grab Animated Region Order Indices if meta data is available
		if (meta_data)
		{
			auto dst_indices = GetIndicesCopy(cur_num_indices);
			auto has_region_order = meta_data->hasRegionOrder(
				creature_manager->GetActiveAnimationName().ToString(),
				(int)creature_manager->getActualRunTime());
			if (shouldSkinSwap() && (has_region_order == false))
			{
				// Skin Swap
				std::copy(
					skin_swap_indices.GetData(),
					skin_swap_indices.GetData() + skin_swap_indices.Num(),
					dst_indices);
			}
			else {
				// Region Layer Ordering Animation
				int cur_runtime = (int)(creature_manager->getActualRunTime());
				region_order_indices_num = meta_data->updateIndicesAndPoints(
					dst_indices,
					cur_creature->GetGlobalIndices(),
					cur_pts,
					delta_z,
					cur_creature->GetTotalNumIndices(),
					cur_creature->GetTotalNumPoints(),
					creature_manager->GetActiveAnimationName().ToString(),
					shouldSkinSwap(),
					skin_swap_region_ids,
					cur_runtime);
			}

			should_update_render_indices = true;
		}
	}
	else {
		// Custom order update
		auto& regions_map = cur_creature->GetRenderComposition()->getRegionsMap();
		int32 indice_idx = 0;
		auto dst_indices = GetIndicesCopy(cur_num_indices);

		for (auto& custom_region_name : region_custom_order)
		{
			auto real_name = custom_region_name;
			if (regions_map.Contains(real_name))
			{
				auto single_region = regions_map[real_name];
				glm::float32 * region_pts = cur_pts + (single_region->getStartPtIndex() * 3);
				for (int32 i = 0; i < single_region->getNumPts(); i++)
				{
					region_pts[2] = region_z;
					region_pts += 3;
				}

				region_z += delta_z;

				// Reorder indices
				auto copy_start_idx = single_region->getStartIndex();
				auto copy_end_idx = single_region->getEndIndex();
				auto copy_num_indices = copy_end_idx - copy_start_idx + 1;

				FMemory::Memcpy(dst_indices + indice_idx, 
					cur_idx + copy_start_idx, 
					sizeof(glm::uint32) * copy_num_indices);

				indice_idx += copy_num_indices;
			}
		}

		should_update_render_indices = true;
	}

	// process the render regions
	ProcessRenderRegions();
}

bool CreatureCore::InitCreatureRender()
{
	FName cur_creature_filename = creature_filename;
	bool init_success = false;
	FName load_filename;
	is_animation_loaded = false;

	//////////////////////////////////////////////////////////////////////////
	//Changed by God of Pen
	//////////////////////////////////////////////////////////////////////////
	if (pJsonData != nullptr)
	{
		if (cur_creature_filename.IsNone())
		{
			cur_creature_filename = creature_asset_filename;
		}

		absolute_creature_filename = cur_creature_filename;
		load_filename = cur_creature_filename;

		// try to load creature
		init_success = CreatureCore::LoadDataPacket(load_filename, pJsonData);
	}
	else{
		FString curCreatureFilenameString = cur_creature_filename.ToString();
		bool does_exist = FPlatformFileManager::Get().GetPlatformFile().FileExists(*curCreatureFilenameString);
		if (!does_exist)
		{
			// see if it is in the content directory
			cur_creature_filename = FName(*(FPaths::ProjectContentDir() + FString(TEXT("/")) + curCreatureFilenameString));
			does_exist = FPlatformFileManager::Get().GetPlatformFile().FileExists(*curCreatureFilenameString);
		}

		if (does_exist)
		{
			absolute_creature_filename = cur_creature_filename;
			load_filename = cur_creature_filename;

			// try to load creature
			CreatureCore::LoadDataPacket(load_filename);
			init_success = true;
		}
		else {

			if (do_file_warning && (!load_filename.IsNone())) {
				UE_LOG(LogTemp, Warning, TEXT("ACreatureActor::BeginPlay() - ERROR! Could not load creature file: %s"), *creature_filename.ToString());
				GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, FString::Printf(TEXT("ACreatureActor::BeginPlay() - ERROR! Could not load creature file: %s"), *creature_filename.ToString()));
			}
		}
	}
	
	if (init_success)
	{
		LoadCreature(load_filename);

		// try to load all animations
		auto all_animation_names = creature_manager->GetCreature()->GetAnimationNames();
		auto first_animation_name = all_animation_names[0];
		for (auto& cur_name : all_animation_names)
		{
			CreatureCore::LoadAnimation(load_filename, cur_name);
			AddLoadedAnimation(load_filename, cur_name);
		}

		auto cur_str = start_animation_name;
		for (auto& cur_name : all_animation_names)
		{
			if (cur_name == cur_str)
			{
				first_animation_name = cur_name;
				break;
			}
		}

		SetActiveAnimation(first_animation_name);

		if (smooth_transitions)
		{
			creature_manager->SetAutoBlending(true);
		}

		FillBoneData();
	}

	is_animation_loaded = true;

	return init_success;
}

void CreatureCore::InitValues()
{
	region_colors_map.Empty();
	meta_data = nullptr;
}

void CreatureCore::FillBoneData()
{
	SCOPE_CYCLE_COUNTER(STAT_CreatureCore_FillBoneData);

	auto  render_composition = creature_manager->GetCreature()->GetRenderComposition();
	auto& bones_map = render_composition->getBonesMap();

	if (bone_data.Num() == 0)
	{
		bone_data.SetNum(bones_map.Num());
	}

	int i = 0;
	for (auto& cur_data : bones_map)
	{
		bone_data[i].name = cur_data.Key;

		auto pt1 = cur_data.Value->getWorldStartPt();
		auto pt2 = cur_data.Value->getWorldEndPt();

		/* Id References
		const int x_id = 0;
		const int y_id = 2;
		const int z_id = 1;
		*/

		bone_data[i].point1 = FVector(pt1.x, pt1.y, pt1.z);
		bone_data[i].point2 = FVector(pt2.x, pt2.y, pt2.z);

		// figure out bone transform
		auto cur_bone = cur_data.Value;
		auto bone_start_pt = pt1;
		auto bone_end_pt = pt2;

		auto bone_vec = bone_end_pt - bone_start_pt;
		auto bone_length = glm::length(bone_vec);
		auto bone_unit_vec = bone_vec / bone_length;

		// quick rotation by 90 degrees
		auto bone_unit_normal_vec = bone_unit_vec;
		bone_unit_normal_vec.x = -bone_unit_vec.y;
		bone_unit_normal_vec.y = bone_unit_vec.x;

		FVector bone_midpt = (bone_data[i].point1 + bone_data[i].point2) * 0.5f;
		FVector bone_axis_x(bone_unit_vec.x, bone_unit_vec.y, 0);
		FVector bone_axis_y(bone_unit_normal_vec.x, bone_unit_normal_vec.y, 0);
		FVector bone_axis_z(0, 0, 1);

		FTransform scaleXform(FVector(0, 0, 0));
		scaleXform.SetScale3D(FVector(bone_length * bone_data_length_factor, bone_data_size, bone_data_size));


		//std::swap(bone_midpt.Y, bone_midpt.Z);

		FTransform fixXform;
		fixXform.SetRotation(FQuat::MakeFromEuler(FVector(-90, 0, 0)));

		FTransform rotXform(bone_axis_x, bone_axis_y, bone_axis_z, FVector(0, 0, 0));

		FTransform posXform, posStartXform, posEndXform;
		posXform.SetTranslation(bone_midpt);
		posStartXform.SetTranslation(bone_data[i].point1);
		posEndXform.SetTranslation(bone_data[i].point2);

		//		bone_data[i].xform = scaleXform * FTransform(bone_axis_x, bone_axis_y, bone_axis_z, bone_midpt);
		bone_data[i].xform = scaleXform  * rotXform  * posXform * fixXform;

		bone_data[i].startXform = scaleXform  * rotXform  * posStartXform * fixXform;
		bone_data[i].endXform = scaleXform  * rotXform  * posEndXform * fixXform;

		i++;
	}

}

void CreatureCore::ParseEvents(float deltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_CreatureCore_ParseEvents);

	float cur_runtime = (creature_manager->getActualRunTime());
	animation_frame = cur_runtime;

	auto load_filename = absolute_creature_filename;

	auto cur_animation_name = creature_manager->GetActiveAnimationName();

	auto cur_token = GetAnimationToken(load_filename, cur_animation_name);
	CreatureModule::CreatureAnimation * cur_animation = NULL;
	if (global_animations.Contains(cur_token))
	{
		cur_animation = global_animations[cur_token].Get();
	}


	if (cur_animation)
	{
		int cur_start_time = cur_animation->getStartTime();
		int cur_end_time = cur_animation->getEndTime();

		float diff_val_start = fabs(cur_runtime - cur_start_time);
		const float cutoff = 0.01f;

		if ((diff_val_start <= cutoff)
			&& !is_looping
			&& !play_start_done
			&& should_play)
		{
			play_start_done = true;
			should_process_animation_start = true;
		}

		if ((cur_runtime + 1.0f >= cur_end_time)
			&& !is_looping
			&& !play_end_done
			&& should_play
			&& deltaTime > 0.0f)
		{
			play_end_done = true;
			should_play = false;
			should_process_animation_end = true;
		}
	}

}

void CreatureCore::ProcessRenderRegions()
{
	auto cur_creature = creature_manager->GetCreature();
	auto& regions_map = cur_creature->GetRenderComposition()->getRegionsMap();
	int num_triangles = cur_creature->GetTotalNumIndices() / 3;

	// process alphas
	if (region_colors.Num() != cur_creature->GetTotalNumPoints())
	{
		region_colors.Init(FColor(255, 255, 255, 255), cur_creature->GetTotalNumPoints());
	}

	// fill up animation alphas
	for (auto& cur_region_pair : regions_map)
	{
		auto cur_region = cur_region_pair.Value;
		auto start_pt_index = cur_region->getStartPtIndex();
		auto end_pt_index = cur_region->getEndPtIndex();
		float opacity = FMath::Clamp(cur_region->getOpacity() / 100.0f, 0.0f, 1.0f);
		uint8 cur_alpha = (uint8)(opacity * 255.0f);
		uint8 cur_r = (uint8)(cur_region->getRed() / 100.0f * opacity * 255.0f);
		uint8 cur_g = (uint8)(cur_region->getGreen() / 100.0f * opacity * 255.0f);
		uint8 cur_b = (uint8)(cur_region->getBlue() / 100.0f * opacity * 255.0f);

		for (auto i = start_pt_index; i <= end_pt_index; i++)
		{
			region_colors[i] = FColor(cur_r, cur_g, cur_b, cur_alpha);
		}
	}

	// user overwrite alphas
	if (region_colors_map.Num() > 0)
	{
		// fill up the alphas for specific regions with alpha overwrites
		for (auto cur_iter : region_colors_map)
		{
			auto cur_name = cur_iter.Key;
			auto cur_alpha = cur_iter.Value.A;

			if (regions_map.Contains(cur_name))
			{
				meshRenderRegion * cur_region = regions_map[cur_name];
				auto start_pt_index = cur_region->getStartPtIndex();
				auto end_pt_index = cur_region->getEndPtIndex();

				for (auto i = start_pt_index; i <= end_pt_index; i++)
				{
					region_colors[i] = FColor(cur_alpha, cur_alpha, cur_alpha, cur_alpha);;
				}
			}
		}
	}
}

bool 
CreatureCore::LoadDataPacket(const FName& filename_in)
{
	if (global_load_data_packets.Contains(filename_in))
	{
		// file already loaded, just return
		return true;
	}
	//////////////////////////////////////////////////////////////////////////
	//Changed!
	//////////////////////////////////////////////////////////////////////////
	TSharedPtr<CreatureModule::CreatureLoadDataPacket> new_packet =
		TSharedPtr<CreatureModule::CreatureLoadDataPacket>(new CreatureModule::CreatureLoadDataPacket());

	// load regular JSON
	CreatureModule::LoadCreatureJSONData(filename_in, *new_packet);
	global_load_data_packets[filename_in] = new_packet;

	return true;
}

bool CreatureCore::LoadDataPacket(const FName& filename_in, FString* pSourceData)
{
	//////////////////////////////////////////////////////////////////////////
	//直接从Data中载入
	if (pSourceData == nullptr)
	{
		return false;
	}
	if (global_load_data_packets.Contains(filename_in))
	{
		// file already loaded, just return
		return true;
	}
	else
	{
		if (pSourceData->Len() == 0)
		{
			return false;
		}

		TSharedPtr<CreatureModule::CreatureLoadDataPacket> new_packet =
			TSharedPtr<CreatureModule::CreatureLoadDataPacket>(new CreatureModule::CreatureLoadDataPacket);

		CreatureModule::LoadCreatureJSONDataFromString(*pSourceData, *new_packet);
		global_load_data_packets.Add(filename_in, new_packet);
	}

	return true;
}

void 
CreatureCore::ClearAllDataPackets()
{
	for (auto& cur_packet : global_load_data_packets)
	{
		cur_packet.Value->allocator.deallocate();
	}

	global_load_data_packets.Empty();
}

void CreatureCore::FreeDataPacket(const FName & filename_in)
{
	if (global_load_data_packets.Contains(filename_in))
	{
		TArray<FName> remove_keys;
		for (auto anim_pair : global_animations)
		{
			auto key_str = anim_pair.Key.ToString();
			if (key_str.StartsWith(filename_in.ToString() + FString("_")))
			{
				remove_keys.Add(anim_pair.Key);
			}
		}

		for (auto cur_key : remove_keys)
		{
			global_animations.Remove(cur_key);
		}

		global_load_data_packets.Remove(filename_in);
	}
}

void 
CreatureCore::LoadAnimation(const FName& filename_in, const FName& name_in)
{
	auto cur_token = GetAnimationToken(filename_in, name_in);
	if (global_animations.Contains(cur_token))
	{
		// animation already exists, just return
		return;
	}

	if (global_load_data_packets.Contains(filename_in) == false)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreatureCore::LoadAnimation() - Loading animation but %s was not loaded!"), *filename_in.ToString());
		return;
	}

	auto load_data = global_load_data_packets[filename_in];

	TSharedPtr<CreatureModule::CreatureAnimation> new_animation =
		TSharedPtr<CreatureModule::CreatureAnimation>(
			new CreatureModule::CreatureAnimation(*load_data, name_in));

	global_animations.Add(cur_token, new_animation);
}

TArray<FProceduralMeshTriangle>&
CreatureCore::LoadCreature(const FName& filename_in)
{
	auto load_data = global_load_data_packets[filename_in];

	TSharedPtr<CreatureModule::Creature> new_creature =
		TSharedPtr<CreatureModule::Creature>(new CreatureModule::Creature(*load_data));

	creature_manager = TSharedPtr<CreatureModule::CreatureManager>(
		new CreatureModule::CreatureManager(new_creature));

	draw_triangles.SetNum(creature_manager->GetCreature()->GetTotalNumIndices() / 3, true);

	return draw_triangles;
}

bool 
CreatureCore::AddLoadedAnimation(const FName& filename_in, const FName& name_in)
{
	auto cur_token = GetAnimationToken(filename_in, name_in);
	if (global_animations.Contains(cur_token))
	{
		creature_manager->AddAnimation(global_animations[cur_token]);
		creature_manager->SetIsPlaying(true);
		creature_manager->SetShouldLoop(is_looping);
		return true;
	}

	return false;
}

CreatureModule::CreatureManager * 
CreatureCore::GetCreatureManager()
{
	return creature_manager.Get();
}

void 
CreatureCore::SetBluePrintActiveAnimation(FName name_in)
{
	SetActiveAnimation(name_in);
}

void 
CreatureCore::SetBluePrintBlendActiveAnimation(FName name_in, float factor)
{
	SetAutoBlendActiveAnimation(name_in, factor);
}

void 
CreatureCore::SetBluePrintAnimationCustomTimeRange(FName name_in, int32 start_time, int32 end_time)
{
	auto cur_creature_manager = GetCreatureManager();
	if (!cur_creature_manager)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreatureCore::SetBluePrintAnimationCustomTimeRange() - ERROR! no CreatureManager"), *name_in.ToString());
		return;
	}

	auto cur_str = name_in;
	auto all_animations = creature_manager->GetAllAnimations();
	if (all_animations.Contains(cur_str))
	{
		all_animations[cur_str]->setStartTime(start_time);
		all_animations[cur_str]->setEndTime(end_time);
	}
}

void CreatureCore::SetTimeScale(float timeScale)
{
	auto cur_creature_manager = GetCreatureManager();
	if (!cur_creature_manager)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreatureCore::SetTimeScale() - ERROR! no CreatureManager"));
		return;
	}

	cur_creature_manager->SetTimeScale(timeScale);
}

void 
CreatureCore::MakeBluePrintPointCache(FName name_in, int32 approximation_level)
{
	auto cur_creature_manager = GetCreatureManager();
	if (!cur_creature_manager)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreatureCore::MakeBluePrintPointCache - ERROR! Could not generate point cache for %s"), *name_in.ToString());
		return;
	}

	int32 real_approximation_level = approximation_level;
	if (real_approximation_level <= 0)
	{
		real_approximation_level = 1;
	}
	else if (real_approximation_level > 10)
	{
		real_approximation_level = 10;
	}

	cur_creature_manager->MakePointCache(name_in, real_approximation_level);
}

void 
CreatureCore::ClearBluePrintPointCache(FName name_in, int32 approximation_level)
{
	auto cur_creature_manager = GetCreatureManager();
	if (!cur_creature_manager)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACreatureActor::MakeBluePrintPointCache() - ERROR! Could not generate point cache for %s"), *name_in.ToString());
		return;
	}

	cur_creature_manager->ClearPointCache(name_in);
}

FTransform 
CreatureCore::GetBluePrintBoneXform(FName name_in, bool world_transform, float position_slide_factor, const FTransform& base_transform) const
{
	FTransform ret_xform;
	for (int32 i = 0; i < bone_data.Num(); i++)
	{
		if (bone_data[i].name == name_in)
		{
			ret_xform = bone_data[i].xform;
			float diff_slide_factor = fabs(position_slide_factor);
			const float diff_cutoff = 0.01f;
			if (diff_slide_factor > diff_cutoff)
			{
				// interpolate between start and end
				ret_xform.Blend(bone_data[i].startXform, bone_data[i].endXform, position_slide_factor + 0.5f);
			}


			if (world_transform)
			{
				FTransform xform = base_transform;
				/*
				FVector world_location = xform.GetTranslation();
				ret_data.point1 = xform.TransformPosition(ret_data.point1);
				ret_data.point2 = xform.TransformPosition(ret_data.point2);
				*/
				//FMatrix no_scale = xform.ToMatrixNoScale();

				ret_xform = ret_xform * xform;
			}

			break;
		}
	}

	return ret_xform;
}

bool 
CreatureCore::IsBluePrintBonesCollide(FVector test_point, float bone_size, const FTransform& base_transform)
{
	if (bone_size <= 0)
	{
		bone_size = 1.0f;
	}

	FTransform xform = base_transform;
	FVector local_test_point = xform.InverseTransformPosition(test_point);
	auto  render_composition = creature_manager->GetCreature()->GetRenderComposition();
	auto& bones_map = render_composition->getBonesMap();

	glm::vec4 real_test_pt(local_test_point.X, local_test_point.Y, local_test_point.Z, 1.0f);
	for (auto cur_data : bones_map)
	{
		auto cur_bone = cur_data.Value;
		auto bone_start_pt = cur_bone->getWorldStartPt();
		auto bone_end_pt = cur_bone->getWorldEndPt();

		auto bone_vec = bone_end_pt - bone_start_pt;
		auto bone_length = glm::length(bone_vec);
		auto bone_unit_vec = bone_vec / bone_length;

		auto rel_vec = real_test_pt - bone_start_pt;
		float proj_length_u = glm::dot(rel_vec, bone_unit_vec);
		if (proj_length_u >= 0 && proj_length_u <= bone_length)
		{
			// quick rotation by 90 degrees
			auto bone_unit_normal_vec = bone_unit_vec;
			bone_unit_normal_vec.x = -bone_unit_vec.y;
			bone_unit_normal_vec.y = bone_unit_vec.x;

			float proj_length_v = fabs(glm::dot(rel_vec, bone_unit_normal_vec));
			if (proj_length_v <= bone_size)
			{
				return true;
			}
		}

	}

	return false;

}

bool 
CreatureCore::RunTick(float delta_time)
{
	SCOPE_CYCLE_COUNTER(STAT_CreatureCore_RunTick);

	if (!is_animation_loaded)
	{
		return false;
	}

	FScopeLock scope_lock(update_lock.Get());

	if (is_driven)
	{
		UpdateCreatureRender();
		FillBoneData();

		return true;
	}

	if (is_disabled)
	{
		return false;
	}

	if (creature_manager.Get())
	{
		ParseEvents(delta_time);

		if (should_play) {
			SCOPE_CYCLE_COUNTER(STAT_CreatureCore_UpdateManager);

			bool morph_targets_valid = false;
			if (run_morph_targets && meta_data) {
				morph_targets_valid = meta_data->morph_data.isValid();
			}

			if (morph_targets_valid) {
				meta_data->updateMorphStep(creature_manager.Get(), delta_time);
			}
			else {
				creature_manager->Update(delta_time);
			}
		}

		UpdateCreatureRender();
		FillBoneData();
	}

	return true;
}

void 
CreatureCore::SetBluePrintAnimationLoop(bool flag_in)
{
	is_looping = flag_in;
	if (creature_manager.Get()) {
		creature_manager->SetShouldLoop(is_looping);
	}
}

void 
CreatureCore::SetBluePrintAnimationPlay(bool flag_in)
{
	should_play = flag_in;
	play_start_done = false;
	play_end_done = false;
}

void 
CreatureCore::SetBluePrintAnimationPlayFromStart()
{
	FScopeLock scope_lock(update_lock.Get());

	SetBluePrintAnimationResetToStart();
	SetBluePrintAnimationPlay(true);
}

void 
CreatureCore::SetBluePrintAnimationResetToStart()
{
	FScopeLock scope_lock(update_lock.Get());

	if (creature_manager.Get()) {
		creature_manager->ResetToStartTimes();
		float cur_runtime = (creature_manager->getActualRunTime());
		animation_frame = cur_runtime;

		creature_manager->Update(0.0f);
	}

	play_start_done = false;
	play_end_done = false;
}

void CreatureCore::SetBluePrintAnimationResetToEnd()
{
	if (creature_manager.Get()) {
		auto *anim = creature_manager->GetAnimation(creature_manager->GetActiveAnimationName());

		float cur_runtime = anim->getEndTime();
		creature_manager->setRunTime(cur_runtime);
		animation_frame = cur_runtime;

		creature_manager->Update(0.0f);
	}

	play_start_done = false;
	play_end_done = false;
}

float
CreatureCore::GetBluePrintAnimationFrame()
{
	return animation_frame;
}

void CreatureCore::SetBluePrintAnimationFrame(float time_in)
{
	auto cur_delta = (time_in - creature_manager->getActualRunTime()) / creature_manager->GetTimeScale();
	creature_manager->Update(cur_delta);
	animation_frame = creature_manager->getActualRunTime();
}

void 
CreatureCore::SetBluePrintRegionAlpha(FName region_name_in, uint8 alpha_in)
{
	if (region_name_in.IsNone())
	{
		return;
	}

	FColor new_color(alpha_in, alpha_in, alpha_in, alpha_in);
	region_colors_map.Add(region_name_in, new_color);
}

void CreatureCore::RemoveBluePrintRegionAlpha(FName region_name_in)
{
	region_colors_map.Remove(region_name_in);
}

void 
CreatureCore::SetBluePrintRegionCustomOrder(TArray<FName> order_in)
{
	region_custom_order = order_in;
}

void 
CreatureCore::ClearBluePrintRegionCustomOrder()
{
	region_custom_order.Empty();
}

void CreatureCore::SetBluePrintRegionItemSwap(FName region_name_in, int32 tag)
{
	creature_manager->GetCreature()->SetActiveItemSwap(region_name_in, tag);
}

void CreatureCore::RemoveBluePrintRegionItemSwap(FName region_name_in)
{
	creature_manager->GetCreature()->RemoveActiveItemSwap(region_name_in);
}

void CreatureCore::SetUseAnchorPoints(bool flag_in)
{
	creature_manager->GetCreature()->SetAnchorPointsActive(flag_in);
}

bool CreatureCore::GetUseAnchorPoints() const
{
	return creature_manager->GetCreature()->GetAnchorPointsActive();
}

void
CreatureCore::SetActiveAnimation(const FName& name_in)
{
	SCOPE_CYCLE_COUNTER(STAT_CreatureCore_SetActiveAnimation);
	creature_manager->SetActiveAnimationName(name_in);
	creature_manager->SetAutoBlending(false);
}

void 
CreatureCore::SetAutoBlendActiveAnimation(const FName& name_in, float factor)
{
	auto all_animations = creature_manager->GetAllAnimations();

	if (all_animations.Contains(name_in) == false)
	{
		return;
	}

	if (factor < 0.001f)
	{
		factor = 0.001f;
	}
	else if (factor > 1.0f)
	{
		factor = 1.0f;
	}

	if (smooth_transitions == false)
	{
		smooth_transitions = true;
	}

	creature_manager->SetAutoBlending(true);
	creature_manager->AutoBlendTo(name_in, factor);
}

void 
CreatureCore::SetIsDisabled(bool flag_in)
{
	is_disabled = flag_in;
}

void 
CreatureCore::SetDriven(bool flag_in)
{
	is_driven = flag_in;
}

bool 
CreatureCore::GetIsReadyPlay() const
{
	return is_ready_play;
}

void CreatureCore::SetGlobalEnablePointCache(bool flag_in)
{
	auto cur_creature_manager = GetCreatureManager();
	if (cur_creature_manager != nullptr)
	{
		cur_creature_manager->SetDoPointCache(flag_in);
	}
}

bool CreatureCore::GetGlobalEnablePointCache()
{
	auto cur_creature_manager = GetCreatureManager();
	return cur_creature_manager->GetDoPointCache();
}

glm::uint32 * CreatureCore::GetIndicesCopy(int init_size)
{
	if (!global_indices_copy)
	{
		global_indices_copy = new glm::uint32[init_size];
	}

	return global_indices_copy;
}

int32 CreatureCore::GetRealTotalIndicesNum() const
{
	if (HasMeshModifier() && mesh_modifier->m_isValid)
	{
		return mesh_modifier->m_numIndices;
	}

	auto cur_creature = creature_manager->GetCreature();
	int32 num_indices = cur_creature->GetTotalNumIndices();

	if (region_order_indices_num > 0)
	{
		num_indices = region_order_indices_num;
	}
	else {
		if (shouldSkinSwap())
		{
			num_indices = skin_swap_indices.Num();
		}
	}

	return num_indices;
}

bool CreatureCore::HasMeshModifier() const
{
	return mesh_modifier.IsValid();
}

void CreatureCore::ClearMeshModifier()
{
	mesh_modifier.Reset();
}

void CreatureCore::UpdateMeshModifier()
{
	if (mesh_modifier.IsValid())
	{
		mesh_modifier->update(*this);
	}
}

std::vector<meshBone *> 
CreatureCore::getAllChildrenWithIgnore(const FName& ignore_name, meshBone * base_bone)
{
	if (base_bone == nullptr)
	{
		base_bone = GetCreatureManager()->GetCreature()->GetRenderComposition()->getRootBone();
	}

	std::vector<meshBone *> ret_data;
	if (base_bone->getKey() == ignore_name)
	{
		return ret_data;
	}

	ret_data.push_back(base_bone);
	for (auto cur_child : base_bone->getChildren())
	{
		std::vector<meshBone *> append_data = getAllChildrenWithIgnore(ignore_name, cur_child);
		ret_data.insert(ret_data.end(), append_data.begin(), append_data.end());
	}

	return ret_data;
}

void CreatureCore::enableSkinSwap(const FString & swap_name_in, bool active)
{
	skin_swap_active = active;
	if (!skin_swap_active)
	{
		skin_swap_indices.Empty();
		skin_swap_name = "";
	}
	else {
		skin_swap_name = swap_name_in;
		if (meta_data)
		{
			auto cur_creature = creature_manager->GetCreature();
			meta_data->buildSkinSwapIndices(
				skin_swap_name,
				cur_creature->GetRenderComposition(),
				skin_swap_indices,
				skin_swap_region_ids);
		}
	}
}

bool CreatureCore::shouldSkinSwap() const
{
	return meta_data && skin_swap_active && (skin_swap_indices.Num() > 0);
}

void CreatureCore::enableRegionColors()
{
	if (meta_data)
	{
		meta_data->updateRegionColors(creature_manager->GetAllAnimations());
	}
}

void 
CreatureCore::RunBeginPlay()
{
	is_ready_play = false;
	InitCreatureRender();
	is_ready_play = true;

	region_colors_map.Empty();
}