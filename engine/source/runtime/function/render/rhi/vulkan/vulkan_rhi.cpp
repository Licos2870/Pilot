#include "runtime/function/render/rhi/vulkan/vulkan_rhi.h"
#include "runtime/function/render/rhi/vulkan/vulkan_util.h"

#include "runtime/function/render/window_system.h"

#include <algorithm>
#include <cassert>

// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define PILOT_XSTR(s) PILOT_STR(s)
#define PILOT_STR(s) #s

#if defined(__GNUC__)
// https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
#if defined(__linux__)
#include <stdlib.h>
#elif defined(__MACH__)
// https://developer.apple.com/library/archive/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html
#include <stdlib.h>
#else
#error Unknown Platform
#endif
#elif defined(_MSC_VER)
// https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros
#include <sdkddkver.h>
#define WIN32_LEAN_AND_MEAN 1
#define NOGDICAPMASKS 1
#define NOVIRTUALKEYCODES 1
#define NOWINMESSAGES 1
#define NOWINSTYLES 1
#define NOSYSMETRICS 1
#define NOMENUS 1
#define NOICONS 1
#define NOKEYSTATES 1
#define NOSYSCOMMANDS 1
#define NORASTEROPS 1
#define NOSHOWWINDOW 1
#define NOATOM 1
#define NOCLIPBOARD 1
#define NOCOLOR 1
#define NOCTLMGR 1
#define NODRAWTEXT 1
#define NOGDI 1
#define NOKERNEL 1
#define NOUSER 1
#define NONLS 1
#define NOMB 1
#define NOMEMMGR 1
#define NOMETAFILE 1
#define NOMINMAX 1
#define NOMSG 1
#define NOOPENFILE 1
#define NOSCROLL 1
#define NOSERVICE 1
#define NOSOUND 1
#define NOTEXTMETRIC 1
#define NOWH 1
#define NOWINOFFSETS 1
#define NOCOMM 1
#define NOKANJI 1
#define NOHELP 1
#define NOPROFILER 1
#define NODEFERWINDOWPOS 1
#define NOMCX 1
#include <Windows.h>
#else
#error Unknown Compiler
#endif

#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace Pilot
{
    VulkanRHI::~VulkanRHI()
    {
        // TODO
    }

    void VulkanRHI::initialize(RHIInitInfo init_info)
    {
        _window = init_info.window_system->getWindow();

        std::array<int, 2> window_size = init_info.window_system->getWindowSize();

        _viewport = {0.0f, 0.0f, (float)window_size[0], (float)window_size[1], 0.0f, 1.0f};
        _scissor  = {{0, 0}, {(uint32_t)window_size[0], (uint32_t)window_size[1]}};

#ifndef NDEBUG
        m_enable_validation_Layers = true;
        m_enable_debug_utils_label = true;
#else
        m_enable_validation_Layers  = false;
        m_enable_debug_utils_label  = false;
#endif

#if defined(__GNUC__) && defined(__MACH__)
        m_enable_point_light_shadow = false;
#else
        m_enable_point_light_shadow = true;
#endif

#if defined(__GNUC__)
        // https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
#if defined(__linux__)
        char const* vk_layer_path = PILOT_XSTR(PILOT_VK_LAYER_PATH);
        setenv("VK_LAYER_PATH", vk_layer_path, 1);
#elif defined(__MACH__)
        // https://developer.apple.com/library/archive/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html
        char const* vk_layer_path    = PILOT_XSTR(PILOT_VK_LAYER_PATH);
        char const* vk_icd_filenames = PILOT_XSTR(PILOT_VK_ICD_FILENAMES);
        setenv("VK_LAYER_PATH", vk_layer_path, 1);
        setenv("VK_ICD_FILENAMES", vk_icd_filenames, 1);
#else
#error Unknown Platform
#endif
#elif defined(_MSC_VER)
        // https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros
        char const* vk_layer_path = PILOT_XSTR(PILOT_VK_LAYER_PATH);
        SetEnvironmentVariableA("VK_LAYER_PATH", vk_layer_path);
        SetEnvironmentVariableA("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", "1");
#else
#error Unknown Compiler
#endif

        createInstance();

        initializeDebugMessenger();

        createWindowSurface();

        initializePhysicalDevice();

        createLogicalDevice();

        createCommandPool();

        createCommandBuffers();

        createDescriptorPool();

        createSyncPrimitives();

        createSwapchain();

        createSwapchainImageViews();

        createFramebufferImageAndView();

        createAssetAllocator();
    }

    void VulkanRHI::prepareContext()
    {
        _p_current_frame_index  = &_current_frame_index;
        _current_command_buffer = _command_buffers[_current_frame_index];
        _p_command_buffers      = _command_buffers;
        _p_command_pools        = _command_pools;
    }

    void VulkanRHI::clear()
    {
        if (m_enable_validation_Layers)
        {
            destroyDebugUtilsMessengerEXT(_instance, m_debug_messenger, nullptr);
        }
    }

    void VulkanRHI::waitForFences()
    {
        VkResult res_wait_for_fences =
            _vkWaitForFences(_device, 1, &_is_frame_in_flight_fences[_current_frame_index], VK_TRUE, UINT64_MAX);
        if (VK_SUCCESS != res_wait_for_fences)
        {
            throw std::runtime_error("failed to synchronize");
        }
    }

    void VulkanRHI::resetCommandPool()
    {
        VkResult res_reset_command_pool = _vkResetCommandPool(_device, _command_pools[_current_frame_index], 0);
        if (VK_SUCCESS != res_reset_command_pool)
        {
            throw std::runtime_error("failed to synchronize");
        }
    }

    bool VulkanRHI::prepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain)
    {
        VkResult acquire_image_result =
            vkAcquireNextImageKHR(_device,
                                  _swapchain,
                                  UINT64_MAX,
                                  _image_available_for_render_semaphores[_current_frame_index],
                                  VK_NULL_HANDLE,
                                  &_current_swapchain_image_index);

        if (VK_ERROR_OUT_OF_DATE_KHR == acquire_image_result)
        {
            recreateSwapchain();
            passUpdateAfterRecreateSwapchain();
            return true;
        }
        else if (VK_SUBOPTIMAL_KHR == acquire_image_result)
        {
            recreateSwapchain();
            passUpdateAfterRecreateSwapchain();

            // NULL submit to wait semaphore
            VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
            VkSubmitInfo         submit_info   = {};
            submit_info.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.waitSemaphoreCount     = 1;
            submit_info.pWaitSemaphores        = &_image_available_for_render_semaphores[_current_frame_index];
            submit_info.pWaitDstStageMask      = wait_stages;
            submit_info.commandBufferCount     = 0;
            submit_info.pCommandBuffers        = NULL;
            submit_info.signalSemaphoreCount   = 0;
            submit_info.pSignalSemaphores      = NULL;

            VkResult res_reset_fences = _vkResetFences(_device, 1, &_is_frame_in_flight_fences[_current_frame_index]);
            assert(VK_SUCCESS == res_reset_fences);

            VkResult res_queue_submit =
                vkQueueSubmit(_graphics_queue, 1, &submit_info, _is_frame_in_flight_fences[_current_frame_index]);
            assert(VK_SUCCESS == res_queue_submit);

            _current_frame_index = (_current_frame_index + 1) % _max_frames_in_flight;
            return true;
        }
        else
        {
            assert(VK_SUCCESS == acquire_image_result);
        }

        // begin command buffer
        VkCommandBufferBeginInfo command_buffer_begin_info {};
        command_buffer_begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        command_buffer_begin_info.flags            = 0;
        command_buffer_begin_info.pInheritanceInfo = nullptr;

        VkResult res_begin_command_buffer =
            _vkBeginCommandBuffer(_command_buffers[_current_frame_index], &command_buffer_begin_info);
        assert(VK_SUCCESS == res_begin_command_buffer);

        return false;
    }

    void VulkanRHI::submitRendering(std::function<void()> passUpdateAfterRecreateSwapchain)
    {
        // end command buffer
        VkResult res_end_command_buffer = _vkEndCommandBuffer(_command_buffers[_current_frame_index]);
        assert(VK_SUCCESS == res_end_command_buffer);

        // submit command buffer
        VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSubmitInfo         submit_info   = {};
        submit_info.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount     = 1;
        submit_info.pWaitSemaphores        = &_image_available_for_render_semaphores[_current_frame_index];
        submit_info.pWaitDstStageMask      = wait_stages;
        submit_info.commandBufferCount     = 1;
        submit_info.pCommandBuffers        = &_command_buffers[_current_frame_index];
        submit_info.signalSemaphoreCount   = 1;
        submit_info.pSignalSemaphores      = &_image_finished_for_presentation_semaphores[_current_frame_index];

        VkResult res_reset_fences = _vkResetFences(_device, 1, &_is_frame_in_flight_fences[_current_frame_index]);
        assert(VK_SUCCESS == res_reset_fences);

        VkResult res_queue_submit =
            vkQueueSubmit(_graphics_queue, 1, &submit_info, _is_frame_in_flight_fences[_current_frame_index]);
        assert(VK_SUCCESS == res_queue_submit);

        // present swapchain
        VkPresentInfoKHR present_info   = {};
        present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores    = &_image_finished_for_presentation_semaphores[_current_frame_index];
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = &_swapchain;
        present_info.pImageIndices      = &_current_swapchain_image_index;

        VkResult present_result = vkQueuePresentKHR(_present_queue, &present_info);
        if (VK_ERROR_OUT_OF_DATE_KHR == present_result || VK_SUBOPTIMAL_KHR == present_result)
        {
            recreateSwapchain();
            passUpdateAfterRecreateSwapchain();
        }
        else
        {
            assert(VK_SUCCESS == present_result);
        }

        _current_frame_index = (_current_frame_index + 1) % _max_frames_in_flight;
    }

    VkCommandBuffer VulkanRHI::beginSingleTimeCommands()
    {
        VkCommandBufferAllocateInfo allocInfo {};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = _command_pool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer command_buffer;
        vkAllocateCommandBuffers(_device, &allocInfo, &command_buffer);

        VkCommandBufferBeginInfo beginInfo {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        _vkBeginCommandBuffer(command_buffer, &beginInfo);

        return command_buffer;
    }

    void VulkanRHI::endSingleTimeCommands(VkCommandBuffer command_buffer)
    {
        _vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submitInfo {};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &command_buffer;

        vkQueueSubmit(_graphics_queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(_graphics_queue);

        vkFreeCommandBuffers(_device, _command_pool, 1, &command_buffer);
    }

    // validation layers
    bool VulkanRHI::checkValidationLayerSupport()
    {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const char* layerName : m_validation_layers)
        {
            bool layerFound = false;

            for (const auto& layerProperties : availableLayers)
            {
                if (strcmp(layerName, layerProperties.layerName) == 0)
                {
                    layerFound = true;
                    break;
                }
            }

            if (!layerFound)
            {
                return false;
            }
        }

        return true;
    }

    std::vector<const char*> VulkanRHI::getRequiredExtensions()
    {
        uint32_t     glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        if (m_enable_validation_Layers || m_enable_debug_utils_label)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        return extensions;
    }

    // debug callback
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                        VkDebugUtilsMessageTypeFlagsEXT,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                        void*)
    {
        std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
        return VK_FALSE;
    }

    void VulkanRHI::populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo)
    {
        createInfo       = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
    }

    void VulkanRHI::createInstance()
    {
        // validation layer will be enabled in debug mode
        if (m_enable_validation_Layers && !checkValidationLayerSupport())
        {
            throw std::runtime_error("validation layers requested, but not available!");
        }

        m_vulkan_api_version = VK_API_VERSION_1_0;

        // app info
        VkApplicationInfo appInfo {};
        appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName   = "pilot_renderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName        = "Pilot";
        appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion         = m_vulkan_api_version;

        // create info
        VkInstanceCreateInfo instance_create_info {};
        instance_create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_create_info.pApplicationInfo = &appInfo; // the appInfo is stored here

        auto extensions                              = getRequiredExtensions();
        instance_create_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        instance_create_info.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo {};
        if (m_enable_validation_Layers)
        {
            instance_create_info.enabledLayerCount   = static_cast<uint32_t>(m_validation_layers.size());
            instance_create_info.ppEnabledLayerNames = m_validation_layers.data();

            populateDebugMessengerCreateInfo(debugCreateInfo);
            instance_create_info.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
        }
        else
        {
            instance_create_info.enabledLayerCount = 0;
            instance_create_info.pNext             = nullptr;
        }

        // create m_vulkan_context._instance
        if (vkCreateInstance(&instance_create_info, nullptr, &_instance) != VK_SUCCESS)
        {
            throw std::runtime_error("vk create instance");
        }
    }

    void VulkanRHI::initializeDebugMessenger()
    {
        if (m_enable_validation_Layers)
        {
            VkDebugUtilsMessengerCreateInfoEXT createInfo;
            populateDebugMessengerCreateInfo(createInfo);
            if (VK_SUCCESS != createDebugUtilsMessengerEXT(_instance, &createInfo, nullptr, &m_debug_messenger))
            {
                throw std::runtime_error("failed to set up debug messenger!");
            }
        }

        if (m_enable_debug_utils_label)
        {
            _vkCmdBeginDebugUtilsLabelEXT =
                (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(_instance, "vkCmdBeginDebugUtilsLabelEXT");
            _vkCmdEndDebugUtilsLabelEXT =
                (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(_instance, "vkCmdEndDebugUtilsLabelEXT");
        }
    }

    void VulkanRHI::createWindowSurface()
    {
        if (glfwCreateWindowSurface(_instance, _window, nullptr, &_surface) != VK_SUCCESS)
        {
            throw std::runtime_error("glfwCreateWindowSurface");
        }
    }

    void VulkanRHI::initializePhysicalDevice()
    {
        uint32_t physical_device_count;
        vkEnumeratePhysicalDevices(_instance, &physical_device_count, nullptr);
        if (physical_device_count == 0)
        {
            throw std::runtime_error("enumerate physical devices");
        }
        else
        {
            // find one device that matches our requirement
            // or find which is the best
            std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
            vkEnumeratePhysicalDevices(_instance, &physical_device_count, physical_devices.data());

            std::vector<std::pair<int, VkPhysicalDevice>> ranked_physical_devices;
            for (const auto& device : physical_devices)
            {
                VkPhysicalDeviceProperties physical_device_properties;
                vkGetPhysicalDeviceProperties(device, &physical_device_properties);
                int score = 0;

                if (physical_device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                {
                    score += 1000;
                }
                else if (physical_device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                {
                    score += 100;
                }

                ranked_physical_devices.push_back({score, device});
            }

            std::sort(ranked_physical_devices.begin(),
                      ranked_physical_devices.end(),
                      [](const std::pair<int, VkPhysicalDevice>& p1, const std::pair<int, VkPhysicalDevice>& p2) {
                          return p1 > p2;
                      });

            for (const auto& device : ranked_physical_devices)
            {
                if (isDeviceSuitable(device.second))
                {
                    _physical_device = device.second;
                    break;
                }
            }

            if (_physical_device == VK_NULL_HANDLE)
            {
                throw std::runtime_error("failed to find suitable physical device");
            }
        }
    }

    // logical device (m_vulkan_context._device : graphic queue, present queue,
    // feature:samplerAnisotropy)
    void VulkanRHI::createLogicalDevice()
    {
        _queue_indices = findQueueFamilies(_physical_device);

        std::vector<VkDeviceQueueCreateInfo> queue_create_infos; // all queues that need to be created
        std::set<uint32_t>                   queue_families = {_queue_indices.graphics_family.value(),
                                             _queue_indices.present_family.value()};

        float queue_priority = 1.0f;
        for (uint32_t queue_family : queue_families) // for every queue family
        {
            // queue create info
            VkDeviceQueueCreateInfo queue_create_info {};
            queue_create_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_create_info.queueFamilyIndex = queue_family;
            queue_create_info.queueCount       = 1;
            queue_create_info.pQueuePriorities = &queue_priority;
            queue_create_infos.push_back(queue_create_info);
        }

        // physical device features
        VkPhysicalDeviceFeatures physical_device_features = {};

        physical_device_features.samplerAnisotropy = VK_TRUE;

        // support inefficient readback storage buffer
        physical_device_features.fragmentStoresAndAtomics = VK_TRUE;

        // support independent blending
        physical_device_features.independentBlend = VK_TRUE;

        // support geometry shader
        if (m_enable_point_light_shadow)
        {
            physical_device_features.geometryShader = VK_TRUE;
        }

        // device create info
        VkDeviceCreateInfo device_create_info {};
        device_create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.pQueueCreateInfos       = queue_create_infos.data();
        device_create_info.queueCreateInfoCount    = static_cast<uint32_t>(queue_create_infos.size());
        device_create_info.pEnabledFeatures        = &physical_device_features;
        device_create_info.enabledExtensionCount   = static_cast<uint32_t>(m_device_extensions.size());
        device_create_info.ppEnabledExtensionNames = m_device_extensions.data();
        device_create_info.enabledLayerCount       = 0;

        if (vkCreateDevice(_physical_device, &device_create_info, nullptr, &_device) != VK_SUCCESS)
        {
            throw std::runtime_error("vk create device");
        }

        // initialize queues of this device
        vkGetDeviceQueue(_device, _queue_indices.graphics_family.value(), 0, &_graphics_queue);
        vkGetDeviceQueue(_device, _queue_indices.present_family.value(), 0, &_present_queue);

        // more efficient pointer
        _vkWaitForFences         = (PFN_vkWaitForFences)vkGetDeviceProcAddr(_device, "vkWaitForFences");
        _vkResetFences           = (PFN_vkResetFences)vkGetDeviceProcAddr(_device, "vkResetFences");
        _vkResetCommandPool      = (PFN_vkResetCommandPool)vkGetDeviceProcAddr(_device, "vkResetCommandPool");
        _vkBeginCommandBuffer    = (PFN_vkBeginCommandBuffer)vkGetDeviceProcAddr(_device, "vkBeginCommandBuffer");
        _vkEndCommandBuffer      = (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(_device, "vkEndCommandBuffer");
        _vkCmdBeginRenderPass    = (PFN_vkCmdBeginRenderPass)vkGetDeviceProcAddr(_device, "vkCmdBeginRenderPass");
        _vkCmdNextSubpass        = (PFN_vkCmdNextSubpass)vkGetDeviceProcAddr(_device, "vkCmdNextSubpass");
        _vkCmdEndRenderPass      = (PFN_vkCmdEndRenderPass)vkGetDeviceProcAddr(_device, "vkCmdEndRenderPass");
        _vkCmdBindPipeline       = (PFN_vkCmdBindPipeline)vkGetDeviceProcAddr(_device, "vkCmdBindPipeline");
        _vkCmdSetViewport        = (PFN_vkCmdSetViewport)vkGetDeviceProcAddr(_device, "vkCmdSetViewport");
        _vkCmdSetScissor         = (PFN_vkCmdSetScissor)vkGetDeviceProcAddr(_device, "vkCmdSetScissor");
        _vkCmdBindVertexBuffers  = (PFN_vkCmdBindVertexBuffers)vkGetDeviceProcAddr(_device, "vkCmdBindVertexBuffers");
        _vkCmdBindIndexBuffer    = (PFN_vkCmdBindIndexBuffer)vkGetDeviceProcAddr(_device, "vkCmdBindIndexBuffer");
        _vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)vkGetDeviceProcAddr(_device, "vkCmdBindDescriptorSets");
        _vkCmdDrawIndexed        = (PFN_vkCmdDrawIndexed)vkGetDeviceProcAddr(_device, "vkCmdDrawIndexed");
        _vkCmdClearAttachments   = (PFN_vkCmdClearAttachments)vkGetDeviceProcAddr(_device, "vkCmdClearAttachments");

        _depth_image_format = findDepthFormat();
    }

    void VulkanRHI::createCommandPool()
    {
        // default graphics command pool
        {
            VkCommandPoolCreateInfo command_pool_create_info {};
            command_pool_create_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            command_pool_create_info.pNext            = NULL;
            command_pool_create_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            command_pool_create_info.queueFamilyIndex = _queue_indices.graphics_family.value();

            if (vkCreateCommandPool(_device, &command_pool_create_info, nullptr, &_command_pool) != VK_SUCCESS)
            {
                throw std::runtime_error("vk create command pool");
            }
        }

        // other command pools
        {
            VkCommandPoolCreateInfo command_pool_create_info;
            command_pool_create_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            command_pool_create_info.pNext            = NULL;
            command_pool_create_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            command_pool_create_info.queueFamilyIndex = _queue_indices.graphics_family.value();

            for (uint32_t i = 0; i < _max_frames_in_flight; ++i)
            {
                if (vkCreateCommandPool(_device, &command_pool_create_info, NULL, &_command_pools[i]) != VK_SUCCESS)
                {
                    throw std::runtime_error("vk create command pool");
                }
            }
        }
    }

    void VulkanRHI::createCommandBuffers()
    {
        VkCommandBufferAllocateInfo command_buffer_allocate_info {};
        command_buffer_allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_buffer_allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_buffer_allocate_info.commandBufferCount = 1U;

        for (uint32_t i = 0; i < _max_frames_in_flight; ++i)
        {
            command_buffer_allocate_info.commandPool = _command_pools[i];

            if (vkAllocateCommandBuffers(_device, &command_buffer_allocate_info, &_command_buffers[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("vk allocate command buffers");
            }
        }
    }

    void VulkanRHI::createDescriptorPool()
    {
        // Since DescriptorSet should be treated as asset in Vulkan, DescriptorPool
        // should be big enough, and thus we can sub-allocate DescriptorSet from
        // DescriptorPool merely as we sub-allocate Buffer/Image from DeviceMemory.

        VkDescriptorPoolSize pool_sizes[5];
        pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        pool_sizes[0].descriptorCount = 3 + 2 + 2 + 2 + 1 + 1 + 3 + 3;
        pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = 1 + 1 + 1 * _max_vertex_blending_mesh_count;
        pool_sizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[2].descriptorCount = 1 * _max_material_count;
        pool_sizes[3].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[3].descriptorCount = 3 + 5 * _max_material_count + 1 + 1; // ImGui_ImplVulkan_CreateDeviceObjects
        pool_sizes[4].type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        pool_sizes[4].descriptorCount = 4 + 1 + 1 + 2;

        VkDescriptorPoolCreateInfo pool_info {};
        pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
        pool_info.pPoolSizes    = pool_sizes;
        pool_info.maxSets =
            1 + 1 + 1 + _max_material_count + _max_vertex_blending_mesh_count + 1 + 1; // +skybox + axis descriptor set
        pool_info.flags = 0U;

        if (vkCreateDescriptorPool(_device, &pool_info, nullptr, &_descriptor_pool) != VK_SUCCESS)
        {
            throw std::runtime_error("create descriptor pool");
        }
    }

    // semaphore : signal an image is ready for rendering // ready for presentation
    // (m_vulkan_context._swapchain_images --> semaphores, fences)
    void VulkanRHI::createSyncPrimitives()
    {
        VkSemaphoreCreateInfo semaphore_create_info {};
        semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fence_create_info {};
        fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // the fence is initialized as signaled

        for (uint32_t i = 0; i < _max_frames_in_flight; i++)
        {
            if (vkCreateSemaphore(
                    _device, &semaphore_create_info, nullptr, &_image_available_for_render_semaphores[i]) !=
                    VK_SUCCESS ||
                vkCreateSemaphore(
                    _device, &semaphore_create_info, nullptr, &_image_finished_for_presentation_semaphores[i]) !=
                    VK_SUCCESS ||
                vkCreateFence(_device, &fence_create_info, nullptr, &_is_frame_in_flight_fences[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("vk create semaphore & fence");
            }
        }
    }

    void VulkanRHI::createFramebufferImageAndView()
    {
        VulkanUtil::createImage(_physical_device,
                                _device,
                                _swapchain_extent.width,
                                _swapchain_extent.height,
                                _depth_image_format,
                                VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                _depth_image,
                                _depth_image_memory,
                                0,
                                1,
                                1);

        _depth_image_view = VulkanUtil::createImageView(
            _device, _depth_image, _depth_image_format, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
    }

    void VulkanRHI::createSwapchainImageViews()
    {
        _swapchain_imageviews.resize(_swapchain_images.size());

        // create imageview (one for each this time) for all swapchain images
        for (size_t i = 0; i < _swapchain_images.size(); i++)
        {
            _swapchain_imageviews[i] = VulkanUtil::createImageView(_device,
                                                                   _swapchain_images[i],
                                                                   _swapchain_image_format,
                                                                   VK_IMAGE_ASPECT_COLOR_BIT,
                                                                   VK_IMAGE_VIEW_TYPE_2D,
                                                                   1,
                                                                   1);
        }
    }

    void VulkanRHI::createAssetAllocator()
    {
        VmaVulkanFunctions vulkanFunctions    = {};
        vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
        vulkanFunctions.vkGetDeviceProcAddr   = &vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.vulkanApiVersion       = m_vulkan_api_version;
        allocatorCreateInfo.physicalDevice         = _physical_device;
        allocatorCreateInfo.device                 = _device;
        allocatorCreateInfo.instance               = _instance;
        allocatorCreateInfo.pVulkanFunctions       = &vulkanFunctions;

        vmaCreateAllocator(&allocatorCreateInfo, &_assets_allocator);
    }

    void VulkanRHI::createSwapchain()
    {
        // query all supports of this physical device
        SwapChainSupportDetails swapchain_support_details = querySwapChainSupport(_physical_device);

        // choose the best or fitting format
        VkSurfaceFormatKHR chosen_surface_format =
            chooseSwapchainSurfaceFormatFromDetails(swapchain_support_details.formats);
        // choose the best or fitting present mode
        VkPresentModeKHR chosen_presentMode =
            chooseSwapchainPresentModeFromDetails(swapchain_support_details.presentModes);
        // choose the best or fitting extent
        VkExtent2D chosen_extent = chooseSwapchainExtentFromDetails(swapchain_support_details.capabilities);

        uint32_t image_count = swapchain_support_details.capabilities.minImageCount + 1;
        if (swapchain_support_details.capabilities.maxImageCount > 0 &&
            image_count > swapchain_support_details.capabilities.maxImageCount)
        {
            image_count = swapchain_support_details.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo {};
        createInfo.sType   = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = _surface;

        createInfo.minImageCount    = image_count;
        createInfo.imageFormat      = chosen_surface_format.format;
        createInfo.imageColorSpace  = chosen_surface_format.colorSpace;
        createInfo.imageExtent      = chosen_extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t queueFamilyIndices[] = {_queue_indices.graphics_family.value(), _queue_indices.present_family.value()};

        if (_queue_indices.graphics_family != _queue_indices.present_family)
        {
            createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices   = queueFamilyIndices;
        }
        else
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform   = swapchain_support_details.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode    = chosen_presentMode;
        createInfo.clipped        = VK_TRUE;

        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(_device, &createInfo, nullptr, &_swapchain) != VK_SUCCESS)
        {
            throw std::runtime_error("vk create swapchain khr");
        }

        vkGetSwapchainImagesKHR(_device, _swapchain, &image_count, nullptr);
        _swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(_device, _swapchain, &image_count, _swapchain_images.data());

        _swapchain_image_format = chosen_surface_format.format;
        _swapchain_extent       = chosen_extent;

        _scissor = {{0, 0}, {_swapchain_extent.width, _swapchain_extent.height}};
    }

    void VulkanRHI::clearSwapchain()
    {
        for (auto imageview : _swapchain_imageviews)
        {
            vkDestroyImageView(_device, imageview, NULL);
        }
        vkDestroySwapchainKHR(_device, _swapchain, NULL); // also swapchain images
    }

    void VulkanRHI::recreateSwapchain()
    {
        int width  = 0;
        int height = 0;
        glfwGetFramebufferSize(_window, &width, &height);
        while (width == 0 || height == 0) // minimized 0,0, pause for now
        {
            glfwGetFramebufferSize(_window, &width, &height);
            glfwWaitEvents();
        }

        VkResult res_wait_for_fences =
            _vkWaitForFences(_device, _max_frames_in_flight, _is_frame_in_flight_fences, VK_TRUE, UINT64_MAX);
        assert(VK_SUCCESS == res_wait_for_fences);

        vkDestroyImageView(_device, _depth_image_view, NULL);
        vkDestroyImage(_device, _depth_image, NULL);
        vkFreeMemory(_device, _depth_image_memory, NULL);

        for (auto imageview : _swapchain_imageviews)
        {
            vkDestroyImageView(_device, imageview, NULL);
        }
        vkDestroySwapchainKHR(_device, _swapchain, NULL);

        createSwapchain();
        createSwapchainImageViews();
        createFramebufferImageAndView();
    }

    VkResult VulkanRHI::createDebugUtilsMessengerEXT(VkInstance                                instance,
                                                     const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                                     const VkAllocationCallbacks*              pAllocator,
                                                     VkDebugUtilsMessengerEXT*                 pDebugMessenger)
    {
        auto func =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (func != nullptr)
        {
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
        }
        else
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    void VulkanRHI::destroyDebugUtilsMessengerEXT(VkInstance                   instance,
                                                  VkDebugUtilsMessengerEXT     debugMessenger,
                                                  const VkAllocationCallbacks* pAllocator)
    {
        auto func =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr)
        {
            func(instance, debugMessenger, pAllocator);
        }
    }

    Pilot::QueueFamilyIndices VulkanRHI::findQueueFamilies(VkPhysicalDevice physical_device) // for device and surface
    {
        QueueFamilyIndices indices;
        uint32_t           queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

        int i = 0;
        for (const auto& queue_family : queue_families)
        {
            if (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) // if support graphics command queue
            {
                indices.graphics_family = i;
            }

            VkBool32 is_present_support = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device,
                                                 i,
                                                 _surface,
                                                 &is_present_support); // if support surface presentation
            if (is_present_support)
            {
                indices.present_family = i;
            }

            if (indices.isComplete())
            {
                break;
            }
            i++;
        }
        return indices;
    }

    bool VulkanRHI::checkDeviceExtensionSupport(VkPhysicalDevice physical_device)
    {
        uint32_t extension_count;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);

        std::vector<VkExtensionProperties> available_extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, available_extensions.data());

        std::set<std::string> required_extensions(m_device_extensions.begin(), m_device_extensions.end());
        for (const auto& extension : available_extensions)
        {
            required_extensions.erase(extension.extensionName);
        }

        return required_extensions.empty();
    }

    bool VulkanRHI::isDeviceSuitable(VkPhysicalDevice physical_device)
    {
        auto queue_indices           = findQueueFamilies(physical_device);
        bool is_extensions_supported = checkDeviceExtensionSupport(physical_device);
        bool is_swapchain_adequate   = false;
        if (is_extensions_supported)
        {
            SwapChainSupportDetails swapchain_support_details = querySwapChainSupport(physical_device);
            is_swapchain_adequate =
                !swapchain_support_details.formats.empty() && !swapchain_support_details.presentModes.empty();
        }

        VkPhysicalDeviceFeatures physical_device_features;
        vkGetPhysicalDeviceFeatures(physical_device, &physical_device_features);

        if (!queue_indices.isComplete() || !is_swapchain_adequate || !physical_device_features.samplerAnisotropy)
        {
            return false;
        }

        return true;
    }

    Pilot::SwapChainSupportDetails VulkanRHI::querySwapChainSupport(VkPhysicalDevice physical_device)
    {
        SwapChainSupportDetails details_result;

        // capabilities
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, _surface, &details_result.capabilities);

        // formats
        uint32_t format_count;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, _surface, &format_count, nullptr);
        if (format_count != 0)
        {
            details_result.formats.resize(format_count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                physical_device, _surface, &format_count, details_result.formats.data());
        }

        // present modes
        uint32_t presentmode_count;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, _surface, &presentmode_count, nullptr);
        if (presentmode_count != 0)
        {
            details_result.presentModes.resize(presentmode_count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                physical_device, _surface, &presentmode_count, details_result.presentModes.data());
        }

        return details_result;
    }

    VkFormat VulkanRHI::findDepthFormat()
    {
        return findSupportedFormat({VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                                   VK_IMAGE_TILING_OPTIMAL,
                                   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    VkFormat VulkanRHI::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                            VkImageTiling                tiling,
                                            VkFormatFeatureFlags         features)
    {
        for (VkFormat format : candidates)
        {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(_physical_device, format, &props);

            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
            {
                return format;
            }
            else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
            {
                return format;
            }
        }

        throw std::runtime_error("findSupportedFormat failed");
    }

    VkSurfaceFormatKHR
    VulkanRHI::chooseSwapchainSurfaceFormatFromDetails(const std::vector<VkSurfaceFormatKHR>& available_surface_formats)
    {
        for (const auto& surface_format : available_surface_formats)
        {
            // TODO: select the VK_FORMAT_B8G8R8A8_SRGB surface format,
            // there is no need to do gamma correction in the fragment shader
            if (surface_format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                return surface_format;
            }
        }
        return available_surface_formats[0];
    }

    VkPresentModeKHR
    VulkanRHI::chooseSwapchainPresentModeFromDetails(const std::vector<VkPresentModeKHR>& available_present_modes)
    {
        for (VkPresentModeKHR present_mode : available_present_modes)
        {
            if (VK_PRESENT_MODE_MAILBOX_KHR == present_mode)
            {
                return VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanRHI::chooseSwapchainExtentFromDetails(const VkSurfaceCapabilitiesKHR& capabilities)
    {
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            return capabilities.currentExtent;
        }
        else
        {
            int width, height;
            glfwGetFramebufferSize(_window, &width, &height);

            VkExtent2D actualExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

            actualExtent.width =
                std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height =
                std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

            return actualExtent;
        }
    }

} // namespace Pilot