// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetworkPredictionTypes.h"
#include "NetworkSimulationModelBuffer.h"
#include "NetworkSimulationModelTypes.h"
#include "NetworkSimulationModelReplicators.h"

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
//	TNetworkedSimulationModel
//	
//	* Has all logic for "ticking, advancing buffers, calling Update, calling ServerRPC etc
//	* Doesn't have anything about update component, movesweep, etc
//	* Concept of "IDriver" which is the owning object that is driving the network sim. This is the interface to the outside UE4 world.
//	* Has 4 buffers:
//		-Input: Generated by a client / not the authority.
//		-Sync: What we are trying to keep in sync. The state that evolves frame to frame with an Update function.
//		-Aux: State that is also an input into the simulation but does not intrinsically evolve from to frame. Changes to this state can be trapped/tracked/predicted.
//		-Debug: Replicated buffer from server->client with server-frame centered debug information. Compiled out of shipping builds.
//
// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

template <
	typename TSimulation,								// Final Simulation class. Used to call static T::Update function.
	typename TInDriver,
	typename TUserBufferTypes,							// The user types (input, sync, aux, debug). Note this gets wrapped in TInternalBufferTypes internally.
	typename InTTickSettings=TNetworkSimTickSettings<>, // Defines global rules about time keeping and ticking

	// Core proxies that dictate how data replicates and how the simulation evolves for the three main roles
	typename TRepProxyServerRPC =	TReplicator_Server		<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings >,
	typename TRepProxyAutonomous =	TReplicator_Autonomous	<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings>,
	typename TRepProxySimulated =	TReplicator_Simulated	<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings>,

	// Defines how replication happens on these special channels, but doesn't dictate how simulation evolves
	typename TRepProxyReplay =		TReplicator_Sequence	<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings, ENetworkSimBufferTypeId::Sync,  3>,
	typename TRepProxyDebug =		TReplicator_Debug		<TInternalBufferTypes<TUserBufferTypes, InTTickSettings>,	InTTickSettings>
>
class TNetworkedSimulationModel : public INetworkSimulationModel
{
public:

	using TDriver = TInDriver;

	using TBufferTypes = TInternalBufferTypes<TUserBufferTypes, InTTickSettings>;
	using TTickSettings = InTTickSettings;

	using TInputCmd = typename TBufferTypes::TInputCmd;
	using TSyncState = typename TBufferTypes::TSyncState;
	using TAuxState = typename TBufferTypes::TAuxState;
	using TDebugState = typename TBufferTypes::TDebugState;

	using TSimTime = FNetworkSimTime;
	using TRealTime = FNetworkSimTime::FRealTime;

	class IDriver
	{
	public:
		virtual FString GetDebugName() const = 0; // Used for debugging. Recommended to emit the simulation name and the actor name/role.

		virtual void InitSyncState(TSyncState& OutSyncState) const = 0;	// Called to create initial value of the sync state.
		virtual void ProduceInput(const FNetworkSimTime, typename TUserBufferTypes::TInputCmd&) = 0; // Called when the sim is ready to process new local input
		virtual void FinalizeFrame(const TSyncState& SyncState) = 0; // Called from the Network Sim at the end of the sim frame when there is new sync data.
	};
	
	TNetworkedSimulationModel(TDriver* InDriver)
	{
		Driver = InDriver;
	}
	virtual ~TNetworkedSimulationModel()
	{
		SetParentSimulation(nullptr);
		ClearAllDependentSimulations();
	}

	void Tick(const FNetSimTickParameters& Parameters) final override
	{
		// Update previous DebugState based on what we (might) have sent *after* our last Tick 
		// (property replication and ServerRPC get sent after the tick, rather than forcing awkward callback into the NetSim post replication, we can check it here)
		if (auto* DebugBuffer = GetLocalDebugBuffer())
		{
			if (TDebugState* const PrevDebugState = DebugBuffer->FindElementByKeyframe(DebugBuffer->GetHeadKeyframe()))
			{
				if (Parameters.Role == ROLE_AutonomousProxy)
				{
					PrevDebugState->LastSentInputKeyframe = RepProxy_ServerRPC.GetLastSerializedKeyframe();
				}
				else if (Parameters.Role == ROLE_Authority)
				{
					PrevDebugState->LastSentInputKeyframe = RepProxy_Autonomous.GetLastSerializedKeyframe();
				}
			}
		}

		// Current frame debug state
		TDebugState* const DebugState = GetNextLocalDebugStateWrite();
		if (DebugState)
		{
			*DebugState = TDebugState();
			DebugState->LocalDeltaTimeSeconds = Parameters.LocalDeltaTimeSeconds;
			DebugState->LocalGFrameNumber = GFrameNumber;
			DebugState->ProcessedKeyframes.Reset();
			
			if (Parameters.Role == ROLE_AutonomousProxy)
			{
				DebugState->LastReceivedInputKeyframe = RepProxy_Autonomous.GetLastSerializedKeyframe();
			}
			else if (Parameters.Role == ROLE_Authority)
			{
				DebugState->LastReceivedInputKeyframe = RepProxy_ServerRPC.GetLastSerializedKeyframe();
			}
		}

		// ----------------------------------------------------------------------------------------------------------------
		//	PreSimTick
		//	This is the beginning of a new frame. PreSimTick will decide if we should take Parameters.LocalDeltaTimeSeconds
		//	and advance the simulation or not. It will also generate new local input if necessary.
		// ----------------------------------------------------------------------------------------------------------------
		switch (Parameters.Role)
		{
			case ROLE_Authority:
				RepProxy_ServerRPC.template PreSimTick<TSimulation, TDriver>(Driver, Buffers, TickInfo, Parameters);
			break;

			case ROLE_AutonomousProxy:
				RepProxy_Autonomous.template PreSimTick<TSimulation, TDriver>(Driver, Buffers, TickInfo, Parameters);
			break;

			case ROLE_SimulatedProxy:
				RepProxy_Simulated.template PreSimTick<TSimulation, TDriver>(Driver, Buffers, TickInfo, Parameters);
			break;
		}

		// -------------------------------------------------------------------------------------------------------------------------------------------------
		//												Input Processing & Simulation Update
		// -------------------------------------------------------------------------------------------------------------------------------------------------
		if (Buffers.Input.GetHeadKeyframe() > Buffers.Sync.GetHeadKeyframe())
		{
			// -------------------------------------------------------------------------------------------------------------
			//	
			//	The SyncedState buffer needs to be in sync here:
			//		-We want it to have a SyncedState, but it may not on the first frame through (thats ok).
			//		-Its HeadKeyframe should be one behind the Keyframe we are about to process.
			//
			//	Note, InputCmds start @ Keyframe=1. The first SyncedState that Update produces will go in KeyFrame=1.
			//	(E.g, InputCmd @ keyframe=X is used to generate MotionState @ keyframe=X)
			//	This means that SyncedState @ keyframe=0 is always created here via InitSyncState.
			//	This also means that we never actually process InputCmd @ keyframe=0. Which is why LastProcessedInputKeyframe is initialized to 0 ("already processed")
			//	and the buffer has an empty element inserted in InitializeForNetworkRole.
			// -------------------------------------------------------------------------------------------------------------

			if (Buffers.Sync.GetHeadKeyframe() != TickInfo.LastProcessedInputKeyframe)
			{
				if (TickInfo.LastProcessedInputKeyframe != 0)
				{
					// This shouldn't happen, but is not fatal. We are reseting the sync state buffer.
					UE_LOG(LogNetworkSim, Warning, TEXT("%s. Break in SyncState continuity. LastProcessedInputKeyframe: %d. SyncBuffer.GetHeadKeyframe(): %d."), *Driver->GetDebugName() , TickInfo.LastProcessedInputKeyframe, Buffers.Sync.GetHeadKeyframe());
				}

				// We need an initial/current state. Get this from the sim driver
				Buffers.Sync.ResetNextHeadKeyframe(TickInfo.LastProcessedInputKeyframe);
				TSyncState* StartingState = Buffers.Sync.GetWriteNext();
				Driver->InitSyncState(*StartingState);

				// Reset time tracking buffer too
				TickInfo.SetTotalProcessedSimulationTime(TickInfo.GetTotalProcessedSimulationTime(), Buffers.Sync.GetHeadKeyframe());
			}
		
			// -------------------------------------------------------------------------------------------------------------
			// Process Input
			// -------------------------------------------------------------------------------------------------------------
			while(true)
			{
				const int32 Keyframe = TickInfo.LastProcessedInputKeyframe+1;
				if (Keyframe > TickInfo.MaxAllowedInputKeyframe)
				{
					break;
				}

				if (TInputCmd* NextCmd = Buffers.Input.FindElementByKeyframe(Keyframe))
				{
					// We have an unprocessed command, do we have enough allotted simulation time to process it?
					if (TickInfo.GetRemainingAllowedSimulationTime() >= NextCmd->GetFrameDeltaTime())
					{
						// -------------------------------------------------------------------------------------------------
						//	The core process input command and call ::Update block!
						// -------------------------------------------------------------------------------------------------
						TSyncState* PrevSyncState = Buffers.Sync.FindElementByKeyframe(TickInfo.LastProcessedInputKeyframe);
						TSyncState* NextSyncState = Buffers.Sync.GetWriteNext();

						check(PrevSyncState != nullptr);
						check(NextSyncState != nullptr);
						check(Buffers.Sync.GetHeadKeyframe() == Keyframe);
				
						if (DebugState)
						{
							DebugState->ProcessedKeyframes.Add(Keyframe);
						}
					
						TAuxState AuxState; // Temp: aux buffer not implemented yet
						TSimulation::Update(Driver, NextCmd->GetFrameDeltaTime().ToRealTimeSeconds(), *NextCmd, *PrevSyncState, *NextSyncState, AuxState);
					
						TickInfo.IncrementTotalProcessedSimulationTime(NextCmd->GetFrameDeltaTime(), Keyframe);
						TickInfo.LastProcessedInputKeyframe = Keyframe;
					}
					else
					{
						break;
					}
				}
				else
				{
					break;
				}
			}
		}

		// -------------------------------------------------------------------------------------------------------------------------------------------------
		//												Post Sim Tick: finalize the frame
		// -------------------------------------------------------------------------------------------------------------------------------------------------

		switch (Parameters.Role)
		{
			case ROLE_Authority:
				RepProxy_ServerRPC.template PostSimTick<TDriver>(Driver, Buffers, TickInfo, Parameters);
			break;

			case ROLE_AutonomousProxy:
				RepProxy_Autonomous.template PostSimTick<TDriver>(Driver, Buffers, TickInfo, Parameters);
			break;

			case ROLE_SimulatedProxy:
				RepProxy_Simulated.template PostSimTick<TDriver>(Driver, Buffers, TickInfo, Parameters);
			break;
		}

		// -------------------------------------------------------------------------------------------------------------------------------------------------
		//														Debug
		// -------------------------------------------------------------------------------------------------------------------------------------------------

		// Finish debug state buffer recording (what the server processed each frame)
		if (DebugState)
		{
			DebugState->LastProcessedKeyframe = TickInfo.LastProcessedInputKeyframe;
			DebugState->HeadKeyframe = Buffers.Input.GetHeadKeyframe();
			DebugState->RemainingAllowedSimulationTimeSeconds = (float)TickInfo.GetRemainingAllowedSimulationTime().ToRealTimeSeconds();
		}

		// Historical data recording (longer buffers for historical reference)
		if (auto* HistoricData = GetHistoricBuffers())
		{
			HistoricData->Input.CopyAndMerge(Buffers.Input);
			HistoricData->Sync.CopyAndMerge(Buffers.Sync);
			HistoricData->Aux.CopyAndMerge(Buffers.Aux);
		}
	}

	virtual void Reconcile(const ENetRole Role) final override
	{
		// --------------------------------------------------------------------------------------------------------------------------
		//	Reconcile
		//	This will eventually be called outside the Tick loop, only after processing a network bunch
		//	Reconcile is about "making things right" after a network update. We are not processing "more" simulation yet.
		// --------------------------------------------------------------------------------------------------------------------------
		switch (Role)
		{
			case ROLE_Authority:
				RepProxy_ServerRPC.template Reconcile<TSimulation, TDriver>(Driver, Buffers, TickInfo);
			break;

			case ROLE_AutonomousProxy:
				RepProxy_Autonomous.template Reconcile<TSimulation, TDriver>(Driver, Buffers, TickInfo);
			break;

			case ROLE_SimulatedProxy:
				RepProxy_Simulated.template Reconcile<TSimulation, TDriver>(Driver, Buffers, TickInfo);
			break;
		}
	}
	
	void InitializeForNetworkRole(const ENetRole Role, const FNetworkSimulationModelInitParameters& Parameters) final override
	{
		Buffers.Input.SetBufferSize(Parameters.InputBufferSize);
		Buffers.Sync.SetBufferSize(Parameters.SyncedBufferSize);
		Buffers.Aux.SetBufferSize(Parameters.AuxBufferSize);

		if (GetLocalDebugBuffer())
		{
			GetLocalDebugBuffer()->SetBufferSize(Parameters.DebugBufferSize);
		}

		if (auto* MyHistoricBuffers = GetHistoricBuffers(true))
		{
			MyHistoricBuffers->Input.SetBufferSize(Parameters.HistoricBufferSize);
			MyHistoricBuffers->Sync.SetBufferSize(Parameters.HistoricBufferSize);
			MyHistoricBuffers->Aux.SetBufferSize(Parameters.HistoricBufferSize);
		}

		TickInfo.InitSimulationTimeBuffer(Parameters.SyncedBufferSize);

		// We want to start with an empty command in the input buffer. The sync buffer will be populated @ frame 0 with the "current" state when we actually sim. This keeps them in sync
		*Buffers.Input.GetWriteNext() = TInputCmd();
	}

	void NetSerializeProxy(EReplicationProxyTarget Target, const FNetSerializeParams& Params) final override
	{
		switch(Target)
		{
		case EReplicationProxyTarget::ServerRPC:
			RepProxy_ServerRPC.NetSerialize(Params, Buffers, TickInfo);
			break;
		case EReplicationProxyTarget::AutonomousProxy:
			RepProxy_Autonomous.NetSerialize(Params, Buffers, TickInfo);
			break;
		case EReplicationProxyTarget::SimulatedProxy:
			RepProxy_Simulated.NetSerialize(Params, Buffers, TickInfo);
			break;
		case EReplicationProxyTarget::Replay:
			RepProxy_Replay.NetSerialize(Params, Buffers, TickInfo);
			break;
		case EReplicationProxyTarget::Debug:
#if NETSIM_MODEL_DEBUG
			RepProxy_Debug.NetSerialize(Params, Buffers, TickInfo);
			break;
#endif
		default:
			checkf(false, TEXT("Unknown: %d"), (int32)Target);
		};
	}

	int32 GetProxyDirtyCount(EReplicationProxyTarget Target) final override
	{
		switch(Target)
		{
		case EReplicationProxyTarget::ServerRPC:
			return RepProxy_ServerRPC.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::AutonomousProxy:
			return RepProxy_Autonomous.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::SimulatedProxy:
			return RepProxy_Simulated.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::Replay:
			return RepProxy_Replay.GetProxyDirtyCount(Buffers);
		case EReplicationProxyTarget::Debug:
#if NETSIM_MODEL_DEBUG
			return RepProxy_Debug.GetProxyDirtyCount(Buffers);
#endif
		default:
			checkf(false, TEXT("Unknown: %d"), (int32)Target);
			return 0;
		};
	}

	ESimulatedUpdateMode GetSimulatedUpdateMode() const
	{
		return RepProxy_Simulated.GetSimulatedUpdateMode();
	}

	// ------------------------------------------------------------------------------------------------------

	void SetParentSimulation(INetworkSimulationModel* Simulation) final override
	{
		if (RepProxy_Simulated.ParentSimulation)
		{
			RepProxy_Simulated.ParentSimulation->RemoveDependentSimulation(this);
		}
		
		RepProxy_Simulated.ParentSimulation = Simulation;
		if (Simulation)
		{
			Simulation->AddDepdentSimulation(this);
		}
	}

	INetworkSimulationModel* GetParentSimulation() const final override
	{
		return RepProxy_Simulated.ParentSimulation;
	}

	void AddDepdentSimulation(INetworkSimulationModel* Simulation) final override
	{
		check(RepProxy_Autonomous.DependentSimulations.Contains(Simulation) == false);
		RepProxy_Autonomous.DependentSimulations.Add(Simulation);
		NotifyDependentSimNeedsReconcile(); // force reconcile on purpose
	}

	void RemoveDependentSimulation(INetworkSimulationModel* Simulation) final override
	{
		RepProxy_Autonomous.DependentSimulations.Remove(Simulation);
	}

	void NotifyDependentSimNeedsReconcile()
	{
		RepProxy_Autonomous.bDependentSimulationNeedsReconcile = true;
	}

	void BeginRollback(const FNetworkSimTime& RollbackDeltaTime, const int32 ParentKeyframe) final override
	{
		RepProxy_Simulated.template DependentRollbackBegin<TSimulation, TDriver>(Driver, Buffers, TickInfo, RollbackDeltaTime, ParentKeyframe);
	}

	void StepRollback(const FNetworkSimTime& Step, const int32 ParentKeyframe, const bool bFinalStep) final override
	{
		RepProxy_Simulated.template DependentRollbackStep<TSimulation, TDriver>(Driver, Buffers, TickInfo, Step, ParentKeyframe, bFinalStep);
	}

	void ClearAllDependentSimulations()
	{
		TArray<INetworkSimulationModel*> LocalList = MoveTemp(RepProxy_Autonomous.DependentSimulations);
		for (INetworkSimulationModel* DependentSim : LocalList)
		{
			DependentSim->SetParentSimulation(nullptr);
		}
	}

	// -------------------------------------------------------------------------------------------------------

	TDriver* Driver = nullptr;	
	TSimulationTickState<TTickSettings> TickInfo;	// Manages simulation time and what inputs we are processed

	TNetworkSimBufferContainer<TBufferTypes> Buffers;

	TRepProxyServerRPC RepProxy_ServerRPC;
	TRepProxyAutonomous RepProxy_Autonomous;
	TRepProxySimulated RepProxy_Simulated;
	TRepProxyReplay RepProxy_Replay;

	// ------------------------------------------------------------------
	// RPC Sending helper: provides basic send frequency settings for tracking when the Server RPC can be invoked.
	// Note that the Driver is the one that must call the RPC, that cannot be rolled into this templated structure.
	// More flexbile/dynamic send rates may be desireable. There is not reason this *has* to be done here, it could
	// completely be tracked at the driver level, but that will also push more boilerplate to that layer for users.
	// ------------------------------------------------------------------

	void SetDesiredServerRPCSendFrequency(float DesiredHz) final override { ServerRPCThresholdTimeSeconds = 1.f / DesiredHz; }
	bool ShouldSendServerRPC(float DeltaTimeSeconds) final override
	{
		// Don't allow a large delta time to pollute the accumulator
		const float CappedDeltaTimeSeconds = FMath::Min<float>(DeltaTimeSeconds, ServerRPCThresholdTimeSeconds);
		ServerRPCAccumulatedTimeSeconds += DeltaTimeSeconds;
		if (ServerRPCAccumulatedTimeSeconds >= ServerRPCThresholdTimeSeconds)
		{
			ServerRPCAccumulatedTimeSeconds -= ServerRPCThresholdTimeSeconds;
			return true;
		}

		return false;
	}

	// Simulation class should have a static const FName GroupName member
	FName GetSimulationGroupName() const final override { return TSimulation::GroupName; }

private:
	float ServerRPCAccumulatedTimeSeconds = 0.f;
	float ServerRPCThresholdTimeSeconds = 1.f / 999.f; // Default is to send at a max of 999hz. This part of the system needs to be build out more (better handling of super high FPS clients and fixed rate servers)

	// ------------------------------------------------------------------
	//	Debugging
	// ------------------------------------------------------------------
public:

#if NETSIM_MODEL_DEBUG
	TReplicationBuffer<TDebugState>* GetLocalDebugBuffer() {	return &Buffers.Debug; }
	TDebugState* GetNextLocalDebugStateWrite() { return Buffers.Debug.GetWriteNext(); }
	TNetworkSimBufferContainer<TBufferTypes>* GetHistoricBuffers(bool bCreate=false)
	{
		if (HistoricBuffers.IsValid() == false && bCreate) { HistoricBuffers.Reset(new TNetworkSimBufferContainer<TBufferTypes>()); }
		return HistoricBuffers.Get();
	}

	TReplicationBuffer<TDebugState>* GetRemoteDebugBuffer() {	return &RepProxy_Debug.ReceivedBuffer; }
#else
	TReplicationBuffer<TDebugState>* GetLocalDebugBuffer() {	return nullptr; }
	TDebugState* GetNextLocalDebugStateWrite() { return nullptr; }
	TNetworkSimBufferContainer<TBufferTypes>* GetHistoricBuffers(bool bCreate=false) { return nullptr; }
	TReplicationBuffer<TDebugState>* GetRemoteDebugBuffer() {	return nullptr; }
#endif

private:

#if NETSIM_MODEL_DEBUG
	TRepProxyDebug RepProxy_Debug;
	TUniquePtr<TNetworkSimBufferContainer<TBufferTypes>> HistoricBuffers;
#endif
};