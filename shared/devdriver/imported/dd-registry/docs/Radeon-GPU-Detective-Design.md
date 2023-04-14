# Radeon GPU Detective Design

- [1. Introduction](#1-introduction)
- [2. Background Details](#2-background-details)
- [3. Major Interfaces & Data](#3-major-interfaces-data)
  - [3.1. PAL](#31-pal)
  - [3.2. User-mode driver (DX12, Vulkan)](#32-user-mode-driver-dx12-vulkan)
  - [3.3. Shader Compiler](#33-shader-compiler)
  - [3.4. Kernel-mode driver](#34-kernel-mode-driver)
  - [3.5. Application](#35-application)
  - [3.6. Driver Tools](#36-driver-tools)
    - [3.6.1. AMDLOG](#361-amdlog)
    - [3.6.2. Tool Module](#362-tool-module)
    - [3.6.3. Infrastructure](#363-infrastructure)
- [4. Detailed Design](#4-detailed-design)
  - [4.1. Execution Markers](#41-execution-markers)
- [5. Implementation Plan](#5-implementation-plan)
- [6. Issues and Limitations](#6-issues-and-limitations)

## 1. Introduction

***Radeon GPU Detective (RGD)*** is a new developer tool and API that
that can be used to retrieve and process AMD GPU crash dumps. The goal
of the tool is to provide developers with information that can help them
understand the cause of GPU crashes and, when possible, point developers
to the specific error directly. Such information includes the type of
the crash (hang, page fault, etc.), the command buffer executing at the
point of crash, what resource or draw call were involved (i.e. how far
into the command buffer), the state of the GPU, etc.

You can view the full charter for RGD v1.0 on Confluence.

## 2. Background Details

In order to classify the type of crash or hang encountered and provide
actionable information to developers it is necessary to collect data
from several different sources. There have been several attempts at
improved crash data collection in the past, though most of these have
generally focused on one functional area: application level, user-mode
driver level, kernel driver level or hardware data. The driver design
for RGD aims to create a communication system that links these
functional areas and allows us to collect and correlate data across any
or all of the functional areas and provide the infrastructure required
to scale as additional use cases and features are added to the tool.

Version 1 of RGD will be focused primarily on robust, reliable execution
markers and collection of basic crash data. The most common types of
crashes - page faults -- will be the first targets. Even narrowing the
scope to only page faults, there are several sub-types of page faults
that have been identified. The goal for RGD v1.0 is to identify a narrow
band of these page faults and provide reliable, consistent information
for that subset. Future versions will add additional page fault use
cases and expand to address other types of crash or hang, but the
primary requirement will always be reliability.

The ddEvent API, owned by the Driver Tools team, is the primary
mechanism that will be used to collect data for the tool.

## 3. Major Interfaces & Data

The overall design goal for RGD is to define the responsibilities for
each functional area and utilize the DevDriver infrastructure, namely
ddEvent, to do the heavy lifting of communication between the
components. This should allow each of the components to handle the
details of the implementation in whatever way works best for their
component and also allow each component to operate independently,
minimizing the required communication between components. It is
therefore critical that the required data and interfaces are clear. The
following sub-sections provide the definition of the responsibilities,
data and interfaces for each of the key components.

### 3.1 PAL

PAL is responsible for outputting the existing RMV data events and
additional crash analysis specific events. The current event provider
that outputs RMV events will be renamed to PalGpuMemoryEventProvider and
a new event provider will be added, called
PalCrashAnalysisEventProvider.

Event output in the user-mode driver will be handled by the ddEvent
system, the primary mechanism being via the `WriteEvent()` and
`WriteEventWithHeader()` functions provided by the
`DevDriver::EventProtocol::BaseEventProvider` base class. In cases where
an event will be logged from within PAL, public functions will be
defined on the EventProvider class specific to each supported event. See
`EventProvider::LogCreateGpuMemoryEvent()` in `eventProvider.h` for a
current example of this in the RMV implementation.

For cases where events will be logged from PAL clients, the events and
their data structures will be defined in `palEventDefs.h`. The events will
then be logged by `IPlatform::LogEvent()` defined in `palPlatform.h`. The
implementation of this function contains a switch statement that routes
the event to the appropriate event provider function. To support logging
events to multiple providers, an additional `providerId` parameter will be
added to `IPlatform::LogEvent()`.

PAL is also responsible for portions of the marker data design. A more
detailed view of this design is included in section 3.6 of this
document.

### 3.2 User-mode driver (DX12, Vulkan)

The user-mode driver layer is responsible for maintaining the current
RMV logging and additional events may be identified specific to crash
analysis. As described in section 3.1, events will be output from this
layer via PAL's `IPlatform::LogEvent()` function.

Additionally, the user-mode driver will be responsible for receiving
application markers and forwarding them to PAL, see section 3.6.

### 3.3 Shader Compiler

There are no Shader Compiler changes currently planned for v1.0 of RGD.
Later versions will likely require changes to facilitate capture of
details to positively identify the root cause of shader hangs and
certain types of page faults.

### 3.4 Kernel-mode driver

The KMD will be responsible for collecting information from the hardware
at the point of the crash. This crash information will be communicated
by the KMD via a new function to write to AMDLOG. Adding a new function,
as opposed to using the existing `WriteAmdLog()`, makes it clear that
these events are being used by production code and cannot be removed or
modified without breaking that production functionality.

The new event write function will operate using the same callback
function pointer mechanism as the existing WriteAmdLog(), as it is also
communicating with the AMDLOG driver:

```cxx
virtual nc_status WriteDevDriverEvent(uint32_t  eventId,
                                      uint8_t*  pBuffer,
                                      uint32_t& bufferSize);
```

A shared header will be created with the event data structures and event
IDs to be used by both KMD and AMDLOG. See section 4.1 for details on
these event definitions.

The kernel will also expose an interface to enable debug hardware
features when an application is running with RGD capture enabled. This
includes VM crash on page fault and shader wait on memory fetch. Ideally
this interface would be called by the tool, via DevDriver in AMDLOG,
though it's unclear if there is a mechanism available for asynchronous
AMDLOG to KMD communication. If this path proves to be non-feasible then
these debug features will be enabled via Escape call from the UMD. In
this case it will be the KMD's responsibility to properly disable the
features after the crash occurs if the UMD process exits without doing
so.

### 3.5 Application

The application will contribute data in the form of command buffer
markers. See section 4.1 for details on this system.

### 3.6 Driver Tools

#### 3.6.1 AMDLOG

Currently there is a portion of DevDriver code that exists within the
graphics KMD. This code establishes a kernel router to enable the local
shared memory transport and implements an EventProvider for outputting
RMV events for page table updates and process create & destroy. Being
part of the KMD is problematic for tools when there is a TDR or hang,
since the driver may be reset and in that case the entire DevDriver
message bus will be destroyed. This is a bigger concern for RGD, which
is specifically targeting the crash and hang use cases that are
problematic for the DevDriver code in KMD.

To allow for tools to continue to operate in the event of a GPU TDR or
hang, the DevDriver infrastructure will be moved to the AMDLOG utility
driver. This is a kernel driver that is not tied to any specific
hardware device and is installed with the AMD driver. Since AMDLOG is
still in kernel space, we can continue to support the shared memory
transport and continue to receive the RMV events from the KMD.

Rather than relying on the existing WriteAmdLog mechanism, we will
create a new, tool-specific callback object interface for capturing
production tool events. This differentiation will allow KMD engineers to
quickly identify which events can be removed or modified (AMDLOG events)
and which will impact production functionality if removed or modified
(DevDriver events). See section 3.4 for the KMD facing function
prototype. The new callback routine added for DevDriver specific
functionality will otherwise generally match the existing callback
routine for AMDLOG events providing an event ID,

#### 3.6.2 Tool Module

Driver Tools team will create a tool module to communicate with the
user-mode driver, AMDLOG and DevToolsRouterModule. This communication
will be via connecting to the set of EventProviders that are responsible
for outputting data from those components. Data collection will begin as
soon as the target application begins driver initialization
(*HaltedOnPlatformInit* state).

equired event providers are: (i) the PAL gpuMemoryEventProvider, (ii) a
new PAL *CrashEventProvider* for crash analysis specific data, (iii) a
*kernelMemoryEventProvider* in AMDLOG, (iv) a new *kernelCrashEventProvider*
in AMDLOG and (v) the ETW event provider in the DevToolsRouterModule.

The data from each EventProvider will be streamed to a temporary file until
a triggering event is received that indicates that a crash or hang occurred.

Due to the indeterminate length of time between application start and
crash, the amount of data collected could be quite large. It may be
necessary to implement a mechanism for filtering/trimming the data
received from the event providers.

Communication with the tool layer will be via the module context
interface. This interface will provide a function that allows the tool
layer to register callbacks that will be called when a crash event
occurs and a second callback that will provide a heartbeat/status to the
tool layer to indicate the status (e.g. waiting for crash event,
collecting crash data, writing crash file). A DDFileWriter structure
will be used to control file writing, similar to the memory trace
module.

#### 3.6.3 Infrastructure

Driver Tools is also responsible for maintaining the existing ddEvent
library and interfaces. Up until this point, there has only been a
single tool that uses ddEvent to capture data (RMV) and only a single
event provider is used in each component to collect RMV data. With the
addition of RGD, we will potentially have both the memory trace module
and new RGD tool module connecting to the event servers in UMD, AMDLOG
and router and RGD will also require connection to multiple event
providers within UMD and AMDLOG. The EventServer class currently only
supports a single event client connection and the tool side
EventStreamer class only supports connecting to a single EventProvider.
EventServer will need to be updated for RGD to allow multiple
connections. This will allow RMV and RGD tool modules to both be active
at the same time and allow RGD to create a dedicated EventStreamer for
each EventProvider being used.

## 4. Detailed Design

### 4.1 Execution Markers

RGD execution markers are intended to provide detailed, reliable
information about which command buffer caused a GPU crash and where in
the command buffer the crash occurred.

At a high level, RGD markers will be implemented as timestamp writes in
each command buffer. Markers can be added by the application, via the,
by the driver (PAL or API layer) and in later versions we anticipate
markers being added at the hardware level for Ray Tracing and GPU work
graph workloads. In order to distinguish between the markers from these
different sources, the top 4 bits of the 32-bit timestamp value will be
used to indicate the source.

|**Bits 31:28**        | **Bits 0:27** |
|----------------------|---------------|
| SOURCE (see Table 2) | VALUE         |

*Table 2 - Execution marker source types*

With each unique command buffer, the marker VALUE will be a
monotonically increasing value, starting at zero.

The SOURCE shall be one of the following:

|**Value** | **Source Name**
|----------| --------------------------------|
| 0        | Application                     |
| 1        | API Layer (i.e. DX12 or Vulkan) |
| 2        | PAL                             |
| 3 - 9    | Reserved for future use         |
| 10 - 15  | Open for developer use          |

*Table 3 - CrashAnalysisExecutionMarkerBegin Event*

The "System" source is used for special case values. A marker with a
source of 15 and a value of `0xAAAAAAA` is the starting value that
timestamp memory will be initialized to, indicating that the timestamp
has not been written to yet. A source 15 and a value of `0xFFFFFFF` will
be used at the end of the command buffer to indicate that all timestamps
have completed for that command buffer. (Represented as a 32-bit
unsigned integer, these markers have the values of `0xFAAAAAAA` and
`0xFFFFFFFF`, respectively.)

Each command buffer, upon creation in the usermode driver, will be
assigned a unique 32-bit ID. A CPU-side buffer is then created for each
command buffer, sized with enough memory to hold the command buffer's ID
and two 32-bit timestamps. This CPU-side buffer is mapped to the GPU's
GART heap memory so that it is accessible to the timestamp writes in the
command buffer and ensuring that the memory values will be accessible
even through a GPU reset. Consequently, this allows for GPU-side writing
and CPU-side reading of timestamp markers.

One of the timestamps will be used for top-of-pipe writes (*BeginMarker*)
and the other for bottom-of-pipe (*EndMarker*). Additional data may be
added to this CPU metadata structure as needed.

When an execution marker is added from the application or driver, an
event will be output through the UM Crash Analysis Event Provider. The
begin event identifies the command buffer, marker value and string, the
end event only provides the command buffer ID and marker value.

| Data Field Name  | Type    | Notes                                          |
| :----------------|:-------:|:-----------------------------------------------|
| CmdBufferId      | uint32  | Unique 32-bit ID for the command buffer the    |
|                  |         | execution maker will be inserted into.         |
| MarkerValue      | uint32  | Value written to the timestamp memory upon     |
|                  |         | execution, see Table 1 for data format.        |
| MarkerStringSize | uint32  | Size of the MarkerString field.                |
| MarkerString     | char*   | Variably sized field that contains the marker  |
|                  |         | string data.                                   |

*Table 3 - CrashAnalysisExecutionMarkerEnd Event*

| Data Field Name  | Type    | Notes                                          |
| :----------------|:-------:|:-----------------------------------------------|
| CmdBufferId      | uint32  | Unique 32-bit ID for the command buffer the    |
|                  |         | execution maker will be inserted into.         |
| MarkerValue      | uint32  | Value written to the timestamp memory upon     |
|                  |         | execution, see Table 1 for data format.        |

*Table 4 - CrashDebugNopData Event*

Events from the UM Crash Analysis Provider will be captured in the
`UMD_CRASH_EVENTS` chunk in the final RDF output file.

At the point of crash or hang, the KMD will determine the currently
running command buffers and output the CPU metadata for those command
buffers. This information will sent through the WriteDevDriverEvent
function pointer to AMDFendr and then output by the KM Crash Analysis
Provider.

| **Data Field Name** | **Type** | **Notes**                                    |
|:--------------------| ---------| ---------------------------------------------|
| CmdBufferId         | uint32   | Command buffer ID of the command buffer      |
|                     |          | executing at the point of crash.             |
| BeginTimestampValue | uint32   | Value contained in the begin (top of pipe)   |
|                     |          | execution marker timestamp memory, this will |
|                     |          | match the MarkerValue field in one of the    |
|                     |          | CrashAnalysisExecutionMarker events output by|
|                     |          | UM Crash Anaysis Event Provider.             |
| EndTimestampValue   | uint32   | Value contained in the end (bottom of pipe)  |
|                     |          | execution marker timestamp memory, this will |
|                     |          | match the MarkerValue field in one of the    |
|                     |          | CrashAnalysisExecutionMarker events output by|
|                     |          | UM Crash Anaysis Event Provider.             |

*Table 7 - TimeDelta Event*

### Execution Marker Metadata

#### User Mode Driver

When the RGD tool module is active and connected to the user-mode driver
it will send a command early in device initialization
(HaltedOnPlatformInit state) that instructs the driver to enable crash
analysis features. This command will be implemented as part of the
DriverUtils RPC service:

```json
"name": "EnableCrashAnalysisMode",
"description": "Informs driver to enable crash analysis mode",
"id": "2",
"returns": false
```

*Table 8 - Timestamp Event*

When crash analysis mode is enabled, the *PalCrashAnalysisEventProvider*
will be registered, and several features will be enabled in
*PalCommandBuffer*. The rest of this section describes those additional
features and assumes crash analysis mode is enabled.

During creation of each command buffer, a unique command buffer ID will
be generated and saved in the command buffer class. Command buffer ID
generation will be handled by the PAL Platform. A uint32 counter will be
set to zero during platform initialization and a new Platform function
added:

```cxx
uint32 Platform::GenerateResourceId();
```

It is expected that in the future additional resources will need to
generate unique IDs in the same way that we require for command buffers
in RGD, so the function name reflects that intended wider usage. The
implementation of GenerateResourceId will simply return an atomic
increment of the counter value to avoid the need to take any locks
during execution.

Each command buffer will own a structure that contains required debug
data that will be sent as CPU-side metadata upon command buffer
submission. For v1 the data format for the structure is:

| **Field**             | **Type** | **Notes**                                                                            |
|:----------------------| ---------|--------------------------------------------------------------------------------------|
| CmdBufferId           | uint32   | Command Buffer Id generated for the command buffer being built                       |
| BeginMarkerTimestamp  | uint32   | 32-bit timestamp memory that will be used for top-of-pipe executionmarker writes     |
| EndMarkerTimestamp    | uint32   | 32-bit timestamp memory that will be used for bottom-of-pipe execution marker writes |

*Table 8 - TimeDelta Event*

The CPU memory addresses for the MarkerTimestamp fields will come from
mapping the GPU memory. It is important that this GPU memory be
allocated as uncached memory, to avoid a known issue with cached writes
not properly being flushed at the point of GPU hang/reset. This GPU
memory address will be used in the new *CrashAnalysisMarkerWrite()*
function that will be added to the command buffer class:

    Result CrashAnalysisMarkerWrite(uint32      markerValue,
                                    const char* pMarkerString);

This function will be responsible for:

- Issue a "write immediate" command to the command buffer which
    updates the MarkerTimestamp memory with the value provided

- Outputting a CrashAnalysisExecutionMarker event (see Table 3)

Another new function will be added to the command buffer class that is
responsible for generating the marker values in the format described in
section 4.1 above. This function will utilize an enum for the source
types and otherwise will atomically increment a uint32 value for the
rest of the value.

```cxx
enum class CrashAnalysisExecutionMarkerSource : uint32
{
  Application = 0,
  Pal         = 1,
  ApiLayer    = 2,
  Hardware    = 3,
  Count       = 4
};
```

```cxx
uint32 GenerateMarkerValue(CrashAnalysisExecutionMarkerSource source)
{
  uint32 markerValue = AtomicIncrement(m_currMarkerValue);
  markerValue = (static_cast<uint32>(source) << 28) | (markerValue & 0x0fffffff);
  return markerValue;
}
```

#### Kernel Mode Driver

At the point of crash, the kernel mode driver will look-up the CPU-side
metadata for the currently in-flight command buffers, and output any
debug data structures contained within the metadata. This data will be
sent to DevDriver in AMDLOG and output as a CrashDebugMarkerData event
(see Table 4).

### Crash Analysis Events

This section details the crash analysis events that will be output by
the driver (user-mode or kernel) through the new crash analysis
EventProviders in PAL and AMDLOG when running with crash analysis
enabled.

#### ddEvent RDF Chunk

A new chunk type will be defined for the output RDF file to contain the
crash events for RGD. It is expected that additional events will be
defined in later versions of RGD and that this chunk type will be reused
for other tools that wish to capture ddEvent based event data. For these
reasons, the chunk is not specific to RGD, but rather is a container for
any events captured through the ddEvent system. The contents of the
chunk can be thought of as an array of events with a standard event
header and different event data payloads based on the event type. This
array of events will be preceded by a top-level header that contains
static information about the rest of the events in the capture.

The DDEventProviderHeader (previously called ddEventRdfChunkHeader)
structure is defined in the dd-registry repo.

Immediately following the DDEventProviderHeader will be an array of
events. Each event will have a DDEventHeader which is defined in the
same dd_event.h header as DDEventProviderHeader.

#### ddEvent Definitions

This section details the events that are relevant to RGD that will be
contained within the ddEvent RDF chunks in the output file. In the
future, these event definitions will be migrated to *dd-registry* repo in
the form of YAML definitions for providers which can be run through a
generator to produce a shared header file with structure definitions.

#### Timestamp and Time Delta

These two events are used to track time in the ddEvent system. For a
full description, reference the ddEvent documentation. A shortened
description is copied here for convenience. Time is tracked by ddEvent
in TimeUnits that are a specific number of clocks. By default, TimeUnits
are set to 32 clocks. So each "tick" in a ddEvent stream spans 32 clocks
from the system.

The ddEvent header contains the base timestamp that subsequent events
will base their delta on. Each event includes a Delta field. This uint8
value represents the number of time units since the last timestamp or
time delta event. We limit the delta field to 8 bits to reduce the
number of bytes required for each individual event. However, during
initial modeling of ddEvent data it was discovered that always using a
full size Timestamp event when the delta bits overflowed caused the
event stream size to grow too large. To solve this problem the ddEvent
system defines a second, smaller timing event: TimeDelta.

The TimeDelta event is a variably sized event that encodes a time delta
of 1-6 bytes.

| **Field Name** | **Type** |  **Description**        |
|:---------------|:---------|:------------------------|
| DeltaBytes     | uint8[]  | Time delta value. The   |
|                |          | size of this field will |
|                |          | be 1-6 bytes, indicated |
|                |          | in the ddEventHeader    |

*Table 9 - TdrSummary Event*

If there is ever enough time elapsed between events that the delta would
exceed the maximum value expressed by a TimeDelta event then a Timestamp
event will be output.

| **Field Name** | **Type** | **Description**         |
|:---------------|:--------:|:------------------------|
| Timestamp      | uint64   | Full timestamp value.   |
| Frequency      | uint64   | Timestamp frequency.    |

*Table 10 - VmPageFault Event*

The full timestamp is the starting point and identifies a specific
moment in time that all following events are based on. Each event
includes a Delta field. This uint8 value represents the number of time
units since the last timestamp or time delta event. We limit the delta
field to 8 bits to reduce the number of bytes required for each
individual event. However, during initial modeling of ddEvent data it
was discovered that always using the full size Timestamp event when the
delta bits overflowed caused the event stream size to grow too large. To
solve this problem the ddEvent system defines a second timing event:
TimeDelta.

The TimeDelta event is a variably sized event that encodes a time delta
of 1-6 bytes.

| **Field Name** | **Type** | **Description**         |
|:---------------|:--------:|:------------------------|
| DeltaBytes     | uint8[]  | Time delta value. The   |
|                |          | size of this field will |
|                |          | be 1-6 bytes, indicated |
|                |          | in the ddEventHeader    |

#### CrashAnalysisExecutionMarker

See Table 3 for details.

#### CrashDebugMarkerData

See Table 4 for details.

#### TdrSummary

This section contains several events based on current AMDLOG data
output. Some additional work will be done during implementation to
validate that this data is accessible and consistent enough to be
included as part of RGD data capture. The data events that are confirmed
will be output when a TDR occurs.

| **Field Name**             | **Type**      |
|:---------------------------|:--------------|
| NodeOrdinal                | `uint32`      |
| KdxSchedulerId             | `uint32`      |
| TdrFence                   | `uint32`      |
| LastSubmittedFence         | `uint32`      |
| LastPreemptionFence        | `uint32`      |
| DependentEngineMask        | `uint64`      |
| NumProcesses               | `uint32`      |
| TdrProcessNames            | `char[3][16]` |
| EngineName                 | `char[6]`     |
| PowerDownLastReportedFence | `uint32`      |
| TdrContextCreationTime     | `char[32]`    |
| TdrContextSubmissionCounts | `uint32[50]`  |
| TdrProcessId               | `uint32[3]`   |

#### VmPageFault

| **Field Name** | **Type**   |
|:---------------|------------|
| HubId          | `uint32`   |
| VmId           | `uint32`   |
| FaultStatus    | `uint32`   |
| FaultedFenceId | `uint32`   |
| ProcessId      | `uint32`   |
| ProcessName    | `char[16]` |
| Timestamp      | `char[32]` |
| FaultVmAddress | `uint64`   |

#### ShaderStatus

| **Field Name** | **Type** |
|:---------------|:---------|
| IsShaderHung   | `bool`   |
| ShaderEngine   | `uint32` |
| ShaderArray    | `uint32` |
| CU             | `uint32` |
| SIMD           | `uint32` |
| Wave           | `uint32` |
| ProgramCounter | `uint64` |
| InstructionDw0 | `uint32` |
| InstructionD21 | `uint32` |
| SQ_WAVE_HW_ID  | `uint32` |
| SPI_DEBUG_BUSY | `uint32` |

## 5. Implementation Plan

The Vulkan API extension for page faults was initially targeted as a
prototype target for RGD. While the plan for the Vulkan extension has
changed to push it after RGD v1.0, the work that overlaps with RGD had
already begun with the porting of DevDriver from KMD into AMDLOG. This
is a key requirement for the RGD design and has the biggest impact on
the rest of the system and so will be completed first.

The remainder of the RGD implementation will be split into roughly two
areas: collection of page fault data and command buffer markers. These
can be implemented in parallel. It\'s important to note that page fault
data collection is largely independent per component, while the marker
implementation will require coordination between layers of the stack.

Tests will be created at the component and system level. Component level
tests will be targeted at a single component (i.e. KMD, PAL, etc) and
will test the data via the interfaces described in section 3. While some
basic validation of data will be done, the focus on these tests will be
on functionality. Does the given component output the expected data
fields. The goal of component level tests will be early detection of
regression in a specific component, ideally to catch regressions prior
to promotion to mainline.

In contrast, system level tests will be higher-level and focus more on
collection of valid data for known, supported use cases. These test
cases will utilize test applications that reproduce the target
crash/hang type and verify that the resulting output file matches what
is expected. This type of test starts with the assumption that all of
the component level tests are passing.

## 6. Issues and Limitations

The current design, encompassing v1.0 and near term follow up version,
is focused on the most common and problematic type of crash and hang
scenarios. BSOD errors are both less frequent (particularly for external
users) and generally easier to debug internally. BSOD errors would also
be significantly more difficult to capture and save data for. For that
reason this design does not consider these errors and significant
changes would need to be made to add that support in the future.

We anticipate there will be significant overlap between parts of the
infrastructure being built for RGD and the infrastructure that would be
required for a shader debugger. We also expect the features of RGD to
move closer toward those of the debugger project and anticipate that the
tools may need to interact or incorporate portions of the other tool.
Key stakeholders for RGD should be included in design and implementation
discussions for the debugger project to avoid unnecessary duplication of
effort.

Finally, we have a relatively aggressive schedule to get something
publicly released for RGD by the end of 2022. The v1.0 design will be
focused on surfacing actionable data for external users. That said, we
will not limit data collection to data only usable by external users. We
recognize that internal users of RGD are a key user base and we expect
that post-v1.0 features will be developed first for internal users. With
that in mind, there may be data collected as part of v1.0 implementation
that is not usable for external users. The data format of the RGD file
should allow for internal users to get at this data.
