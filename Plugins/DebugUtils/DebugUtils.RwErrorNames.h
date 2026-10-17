// Generated from RenderWare errcom.def / errcore.def (RenderWare SDK, MIT).
// Do not edit by hand; regenerate with tools/Generate-RwErrorNames.py.
//
// RwErrorCodeCommon: code = 0x80000000 + index (index 0 is E_RW_NOERROR).
// RwErrorCodePlugin_errcore: code = index into errcore.def, for the core
// plugin (pluginID 0x01).
#pragma once

namespace DebugUtilsRwErrorNames
{
    struct Entry { unsigned int code; const char* name; const char* text; };

    // Common errors (errorCode has the MSB set).
    inline constexpr Entry kNames[] = {
        { 0x80000000, "E_RW_NOERROR", "No error" },
        { 0x80000001, "E_RW_BADENGINESTATE", "Engine in incorrect state for this operation" },
        { 0x80000002, "E_RW_BADOPEN", "Error opening the file %s" },
        { 0x80000003, "E_RW_BADPARAM", "Invalid Parameter passed. %s" },
        { 0x80000004, "E_RW_BADVERSION", "The binary file format version is incompatible with this library" },
        { 0x80000005, "E_RW_DEBUGSTACK", "Debug Library has Stack Depth mismatch" },
        { 0x80000006, "E_RW_DEFAULTPIPELINECREATION", "Creation of a default pipeline (%s) failed" },
        { 0x80000007, "E_RW_FRAMENOMATRIX", "The frame does not have an associated matrix" },
        { 0x80000008, "E_RW_INVIMAGEDEPTH", "Invalid Image Depth" },
        { 0x80000009, "E_RW_INVIMAGEFORMAT", "Image has no pixel memory allocated" },
        { 0x8000000A, "E_RW_INVIMAGEMASK", "The mask and image are not the same size" },
        { 0x8000000B, "E_RW_INVIMAGESIZE", "Destination and source images are of differing sizes" },
        { 0x8000000C, "E_RW_INVRASTERDEPTH", "Invalid Raster depth" },
        { 0x8000000D, "E_RW_INVRASTERFORMAT", "Unrecognized raster format" },
        { 0x8000000E, "E_RW_INVRASTERLOCKREQ", "Invalid Raster lock request" },
        { 0x8000000F, "E_RW_INVRASTERMIPLEVEL", "Invalid Raster mipmap level" },
        { 0x80000010, "E_RW_INVRASTERSIZE", "Invalid Raster size" },
        { 0x80000011, "E_RW_INVRASTERUNLOCKREQ", "Invalid Raster unlock request" },
        { 0x80000012, "E_RW_NOFRAME", "Unable to find Frame" },
        { 0x80000013, "E_RW_NOMEM", "Unable to allocate %d bytes of memory" },
        { 0x80000014, "E_RW_NOMIPMAPGENERATIONCALLBACK", "No Mipmap generation callback set - use RtMipmapUseDefaultMipmapGenerationCallback" },
        { 0x80000015, "E_RW_NOTSSEENABLEDCPU", "Not SSE enabled CPU" },
        { 0x80000016, "E_RW_NULLP", "NULL pointer passed to library routine" },
        { 0x80000017, "E_RW_PLUGININIT", "Plugin has already been initialized" },
        { 0x80000018, "E_RW_PLUGINNOTINIT", "Plugin not initialized" },
        { 0x80000019, "E_RW_RANGE", "A supplied parameter was outside the expected range" },
        { 0x8000001A, "E_RW_READ", "Read error on stream" },
        { 0x8000001B, "E_RW_REDUNDANT_FUNCTION", "Call to redundant function - scheduled to be dropped from future releases" },
        { 0x8000001C, "E_RW_WRITE", "Write error on stream" },
        { 0x8000001D, "E_RX_MESHES_RANGES_OVERLAP", "\\n Geometry is in an invalid format for RxPipeline rendering.\\n There may be visible artifacts and/or decreased performance.\\n Use RpGeometrySortByMaterial.\\n [stream %p type %s]" },
        { 0x8000001E, "E_RW_STRING_TRUNCATION", "strlen(%s) >= %d; truncating at character #%d == %c" },
    };
    inline constexpr unsigned kCount = sizeof(kNames) / sizeof(kNames[0]);

    // RenderWare core plugin (pluginID 0x01) errors: code = index.
    inline constexpr unsigned int kCorePluginId = 0x01;
    inline constexpr Entry kCoreNames[] = {
        { 0x0000, "E_RW_BADWINDOW", "Invalid view window dimensions supplied" },
        { 0x0001, "E_RW_CHUNKTYPEGET", "Unable to get a chunk of the given type" },
        { 0x0002, "E_RW_DEVICEERROR", "Device specific error: %s" },
        { 0x0003, "E_RW_DEVICEOPEN", "Request to open device system refused" },
        { 0x0004, "E_RW_DEVICESTART", "Attempt to start Device failed" },
        { 0x0005, "E_RW_ENDOFSTREAM", "At the end of the stream." },
        { 0x0006, "E_RW_FRAMEDESTROYSTATIC", "RwFrameDestroy called on static frame." },
        { 0x0007, "E_RW_FRAMEBUFFERMISMATCH", "Resolutions of parent rasters of frame buffer and Z buffer are different" },
        { 0x0008, "E_RW_FREELISTFREED", "Free List value already on the free list" },
        { 0x0009, "E_RW_FREELISTINVADDRESS", "Invalid Free List memory address" },
        { 0x000A, "E_RW_FREELISTINVMEMBOUND", "Invalid Free List memory boundary" },
        { 0x000B, "E_RW_FREELISTTRASH", "An unused Free List entry has been overwritten" },
        { 0x000C, "E_RW_INSUFFICIENTRESOURCES", "Insufficient resources to satisfy the allocation of %d bytes." },
        { 0x000D, "E_RW_INVSTREAMACCESSTYPE", "Invalid stream access type." },
        { 0x000E, "E_RW_INVSTREAMTYPE", "Invalid stream type." },
        { 0x000F, "E_RW_NEGCLIPPLANE", "Negative positioned clip planes are invalid" },
        { 0x0010, "E_RW_NOCAMERA", "Cannot render until a Camera has been created" },
        { 0x0011, "E_RW_NOFRAMEBUFFER", "Camera has no frame buffer raster" },
        { 0x0012, "E_RW_NOPARENT", "The given object has no parent" },
        { 0x0013, "E_RW_RASTERRECT", "Rectangle is not totally within Raster" },
        { 0x0014, "E_RW_RASTERSTACK", "Insufficient Raster stack space available" },
        { 0x0015, "E_RW_RASTERSTACKEMPTY", "No Raster Currently on Stack" },
        { 0x0016, "E_RW_READTEXMASK", "Unable to read Texture %s / Mask %s" },
        { 0x0017, "E_RW_STREAMOPEN", "Unable to open stream : %s" },
        { 0x0018, "E_RW_SYSTEMREQUEST", "A system request call has failed, request code : 0x%x" },
        { 0x0019, "E_RW_ZEROLENGTH", "A Vector of Zero Length was passed for normalizing" },
        { 0x001A, "E_RX_CANNOT_TRANSFER_BETWEEN_NODES", "Node %s cannot output to node %s" },
        { 0x001B, "E_RX_CANNOT_TRANSFER_FROM_NODE_TO_PIPELINE", "Node %s cannot output to specified pipeline" },
        { 0x001C, "E_RX_CYCLICPIPELINE", "Pipeline contains cycles; illegal" },
        { 0x001D, "E_RX_DEP_DEPENDENCIESMISMATCH", "\\n *** dependencies cannot be satisfied.\\n *** rxCLREQ_REQUIRED on cluster %s, originating\\n *** with node %s, not serviced by a rxCLVALID_VALID -\\n *** blocked at node %s, output #%ld (\\ )." },
        { 0x001E, "E_RX_DEP_DUPLICATECLUSTERDEFS", "\\n *** Node %s specifies RxClusterDefinition for cluster %s more than once in\\n *** clusters of interest array. Distinct clusters within a pipeline must reference\\n *** distinct RxClusterDefinitions, even if the clusters contain the same data type\\n" },
        { 0x001F, "E_RX_DEP_NULLCLUSTERDEF", "Node %s specified with RxClusterDefinition pointer NULL for cluster of interest %d\\n" },
        { 0x0020, "E_RX_DEP_OUTOFMEMORY", "Dependency chasing; memory alloc failed" },
        { 0x0021, "E_RX_EMPTYPIPELINE", "RwPipeline2Execute cannot execute a pipeline with no nodes :)" },
        { 0x0022, "E_RX_FRAGMENTEDPIPELINE", "Pipeline forms two or more unconnected graphs; illegal" },
        { 0x0023, "E_RX_IM3DNOTACTIVE", "Cannot render Im3D primitives outside of a RwIm3dTransform()/RwIm3dEnd() pair" },
        { 0x0024, "E_RX_INVALIDENTRYPOINT", "Pipeline has an invalid entry point" },
        { 0x0025, "E_RX_INVALIDPRIMTYPE", "Unknown primitive type %d" },
        { 0x0026, "E_RX_INVALIDRESUMEPIPELINE", "RwPipeline2Execute cannot resume a different pipeline to the one previously interrupted" },
        { 0x0027, "E_RX_LOCKEDPIPE", "Illegal operation on a locked pipeline" },
        { 0x0028, "E_RX_NODETOOMANYCLUSTERSOFINTEREST", "Node contains more than RXNODEMAXCLUSTERSOFINTEREST clusters of interest; illegal" },
        { 0x0029, "E_RX_NODETOOMANYOUTPUTS", "Node contains more than RXNODEMAXOUTPUTS outputs; illegal" },
        { 0x002A, "E_RX_PIPELINETOOMANYNODES", "Maximum nodes per pipeline exceeded! You may increase the limit by changing the value of _rxPipelineMaxNodes BEFORE starting RenderWare" },
        { 0x002B, "E_RX_NODE_EXECUTION_FAILED", "Node execution failed - %s" },
        { 0x002C, "E_RX_NOVERTS", "Cannot render Im3D primitive - not enough vertices transformed" },
        { 0x002D, "E_RX_PACKETPTRINVALID", "Value of input/output interruption packets pointer not as expected" },
        { 0x002E, "E_RX_PACKETSCOPYFAILED", "Failed to make copies of packets input to RxPipelineExecute()" },
        { 0x002F, "E_RX_RUNFROMNOTVALID", "RunFrom node not a member of the specified pipeline" },
        { 0x0030, "E_RX_RUNTOANDRUNFROM", "RwPipeline2Execute cannot accept both RunTo *and* RunFrom" },
        { 0x0031, "E_RX_RUNTONOTVALID", "RunTo node not a member of the specified pipeline" },
        { 0x0032, "E_RX_TOOMANYVERTS", "More than 65536 vertices passed to RwIm3DTransform; illegal" },
        { 0x0033, "E_RX_UNFINISHEDPIPELINE", "RwPipeline2Execute must resume and finish an interrupted pipeline" },
        { 0x0034, "E_RX_UNLOCKEDPIPE", "Illegal operation on an unlocked pipeline" },
        { 0x0035, "E_RX_UNPROCESSED_PACKETS", "Unprocessed packets found in finished execution context" },
        { 0x0036, "E_RX_UNSATISFIED_REQUIREMENTS", "Cannot send packet between pipelines, requirements not satisfied. Cluster '%s' is missing" },
    };
    inline constexpr unsigned kCoreCount =
        sizeof(kCoreNames) / sizeof(kCoreNames[0]);
}
