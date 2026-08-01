#include "physics-interaction/collision/CollisionLayerPolicy.h"
#include "physics-interaction/grab/ExternalHeldBodyRegistry.h"
#include "physics-interaction/NativeMeleeSuppressionPolicy.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace rock::collision_layer_policy;

    {
        rock::external_held_body_registry::Registry<4> registry;
        int worldAStorage = 0;
        int worldBStorage = 0;
        const void* const worldA = &worldAStorage;
        const void* const worldB = &worldBStorage;
        const std::array<std::uint32_t, 4> rightBodies{
            3490u,
            3491u,
            3490u,
            0x7FFFFFFFu
        };
        registry.publish(
            false,
            worldA,
            rightBodies.data(),
            rightBodies.size());

        Require(registry.any(),
            "an exact held-body publication must make the registry active");
        Require(registry.contains(worldA, 3490u) &&
                    registry.contains(worldA, 3491u),
            "every valid multipart held body must be matched");
        Require(!registry.contains(worldA, 3492u),
            "an unregistered body in the same world must retain player collision");
        Require(!registry.contains(worldB, 3490u),
            "the same raw body ID in another world must retain player collision");
        Require(!registry.contains(worldA, 0x7FFFFFFFu),
            "invalid body IDs must never enter the registry");

        const std::array<std::uint32_t, 1> leftBodies{ 777u };
        registry.publish(
            true,
            worldA,
            leftBodies.data(),
            leftBodies.size());
        Require(registry.contains(worldA, 777u) &&
                    registry.contains(worldA, 3490u),
            "the two physical-hand snapshots must coexist");
        Require(registry.ownerMask(worldA, 3490u) == 1u &&
                    registry.ownerMask(worldA, 777u) == 2u,
            "the registry must preserve exact right/left held ownership");

        // Handoff order: publish the promoted hand before clearing the old
        // owner, so the held object remains protected throughout.
        registry.publish(
            true,
            worldA,
            rightBodies.data(),
            2u);
        Require(registry.ownerMask(worldA, 3490u) == 3u,
            "an atomic handover may briefly publish both exact owners");
        registry.clear(false);
        Require(registry.ownerMask(worldA, 3490u) == 2u,
            "clearing the source hand must preserve destination ownership");
        Require(registry.contains(worldA, 3490u) &&
                    registry.contains(worldA, 3491u),
            "a promoted-hand publication must survive old-owner cleanup");
        registry.clear(true);
        Require(!registry.any(),
            "clearing both hand snapshots must remove all held identities");
    }

    Require(!kAllowGlobalNativePlayerBodySuppression,
        "ROCK must never set a global no-collide bit on native player bodies");

    for (const std::uint32_t layer : {
             FO4_LAYER_PROJECTILE,
             FO4_LAYER_CONEPROJECTILE,
             FO4_LAYER_SPELL,
             FO4_LAYER_SPELLEXPLOSION,
         }) {
        Require(isNativeDamageDeliveryLayer(layer),
            "all native projectile/spell delivery layers must be classified as damage authority");

        const auto decision = evaluatePlayerCharacterControllerContact(
            PlayerCharacterControllerContactPolicyInput{
                .filterEnabled = true,
                .playerController = true,
                .targetLayerKnown = true,
                .targetLayer = layer,
            });
        Require(!decision.suppress,
            "the player character-controller filter must preserve native damage delivery contacts");
        Require(std::string_view(decision.reason) == "nativeDamageAuthority",
            "preserved native damage contacts must report the authority reason");
    }

    Require(!isNativeDamageDeliveryLayer(FO4_LAYER_CLUTTER),
        "ordinary clutter must not be classified as damage delivery");
    Require(!evaluatePlayerCharacterControllerContact(
                 PlayerCharacterControllerContactPolicyInput{
                     .filterEnabled = true,
                     .playerController = true,
                     .targetLayerKnown = true,
                     .targetLayer = FO4_LAYER_CLUTTER,
                 })
                 .suppress,
        "ordinary native clutter must retain vanilla player-body collision");
    Require(!evaluatePlayerCharacterControllerContact(
                 PlayerCharacterControllerContactPolicyInput{
                     .filterEnabled = true,
                     .playerController = true,
                     .targetLayerKnown = true,
                     .targetLayer = FO4_LAYER_STATIC,
                 })
                 .suppress,
        "native world support must remain intact");

    {
        const auto heldNoControllerDecision =
            evaluatePlayerCharacterControllerContact(
                PlayerCharacterControllerContactPolicyInput{
                    .filterEnabled = true,
                    .playerController = true,
                    .targetLayerKnown = true,
                    .targetLayer = FO4_LAYER_BIPED_NO_CC,
                });
        Require(heldNoControllerDecision.suppress,
            "the explicit BIPED_NO_CC held-body layer must be rejected by the per-contact fallback too");
        Require(std::string_view(heldNoControllerDecision.reason) ==
                    "heldNoCharacterController",
            "BIPED_NO_CC rejection must report its narrow held-body reason");
    }

    for (const std::uint32_t layer : {
             FO4_LAYER_CLUTTER,
             FO4_LAYER_WEAPON,
             FO4_LAYER_DEBRIS_SMALL,
             FO4_LAYER_DEBRIS_LARGE,
             FO4_LAYER_SHELLCASING,
             FO4_LAYER_CLUTTER_LARGE,
         }) {
        const auto nativePropDecision = evaluatePlayerCharacterControllerContact(
            PlayerCharacterControllerContactPolicyInput{
                .filterEnabled = true,
                .playerController = true,
                .targetLayerKnown = true,
                .targetLayer = layer,
            });
        Require(!nativePropDecision.suppress,
            "every native small-prop and large-object wrapper must retain vanilla player collision");
        Require(std::string_view(nativePropDecision.reason) == "nativeBody",
            "preserved native prop contacts must report native-body authority");
    }

    Require(nativeCharacterControllerSuppressedLayerMask(true) ==
                layerBitOrZero(FO4_LAYER_BIPED_NO_CC),
        "only the explicit BIPED_NO_CC opt-in layer may suppress character-controller contact");
    Require(nativeCharacterControllerSuppressedLayerMask(false) ==
                layerBitOrZero(FO4_LAYER_BIPED_NO_CC),
        "BIPED_NO_CC must remain independent of the optional large-object classifier");

    for (const std::uint32_t generatedLayer : {
             ROCK_LAYER_HAND,
             ROCK_LAYER_WEAPON,
             ROCK_LAYER_BODY,
             ROCK_LAYER_DYNAMIC_HAND_PROXY,
         }) {
        const auto generatedDecision = evaluatePlayerCharacterControllerContact(
            PlayerCharacterControllerContactPolicyInput{
                .filterEnabled = true,
                .playerController = true,
                .targetLayerKnown = true,
                .targetLayer = generatedLayer,
            });
        Require(generatedDecision.suppress,
            "generated ROCK collider layers must stay out of the native player solver");
        Require(std::string_view(generatedDecision.reason) == "rockGeneratedBody",
            "generated ROCK collider suppression must report the generated-body reason");
    }

    Require(evaluatePlayerCharacterControllerContact(
                PlayerCharacterControllerContactPolicyInput{
                    .filterEnabled = true,
                    .playerController = true,
                    .targetLayerKnown = false,
                })
                .suppress,
        "an unknown/transient target must fail safely before the native solver");

    std::array<std::uint64_t, FO4_LAYER_MATRIX_ADDRESSABLE_COUNT> matrix{};
    matrix.fill(~std::uint64_t{ 0 });
    for (const std::uint32_t dynamicLayer : {
             FO4_LAYER_CLUTTER,
             FO4_LAYER_WEAPON,
             FO4_LAYER_DEBRIS_SMALL,
             FO4_LAYER_DEBRIS_LARGE,
             FO4_LAYER_SHELLCASING,
             FO4_LAYER_CLUTTER_LARGE,
         }) {
        setPair(
            matrix.data(),
            FO4_LAYER_CHARCONTROLLER,
            dynamicLayer,
            false);
    }

    // The generated ROCK body is intentionally not a projectile hitbox.
    // Native BIPED bodies therefore have to retain every damage-delivery pair.
    applyRockGeneratedLayerPolicies(
        matrix.data(),
        true,
        true,
        true,
        false,
        false);
    applyNativeCharacterControllerObjectSuppressionPolicy(
        matrix.data(),
        true,
        ~std::uint64_t{ 0 },
        true);

    Require(layerPairSymmetricMatches(
                matrix.data(),
                FO4_LAYER_CHARCONTROLLER,
                FO4_LAYER_BIPED_NO_CC,
                false),
        "held bodies leased to BIPED_NO_CC must never feed character-controller push constraints");

    for (const std::uint32_t dynamicLayer : {
             FO4_LAYER_CLUTTER,
             FO4_LAYER_WEAPON,
             FO4_LAYER_DEBRIS_SMALL,
             FO4_LAYER_DEBRIS_LARGE,
             FO4_LAYER_SHELLCASING,
             FO4_LAYER_CLUTTER_LARGE,
         }) {
        Require(layerPairSymmetricMatches(
                    matrix.data(),
                    FO4_LAYER_CHARCONTROLLER,
                    dynamicLayer,
                    true),
            "registration must actively restore native player collision for every small-prop and car wrapper layer");
    }

    for (const bool largeObjectClassifierEnabled : { false, true }) {
        auto nativeMatrix = matrix;
        applyNativeCharacterControllerObjectSuppressionPolicy(
            nativeMatrix.data(),
            true,
            ~std::uint64_t{ 0 },
            largeObjectClassifierEnabled);
        for (const std::uint32_t dynamicLayer : {
                 FO4_LAYER_CLUTTER,
                 FO4_LAYER_WEAPON,
                 FO4_LAYER_DEBRIS_SMALL,
                 FO4_LAYER_DEBRIS_LARGE,
                 FO4_LAYER_SHELLCASING,
                 FO4_LAYER_CLUTTER_LARGE,
             }) {
            Require(layerPairSymmetricMatches(
                        nativeMatrix.data(),
                        FO4_LAYER_CHARCONTROLLER,
                        dynamicLayer,
                        true),
                "the matrix policy must preserve native player collision for small props and cars");
        }
    }

    for (const std::uint32_t bipedLayer : {
             FO4_LAYER_BIPED,
             FO4_LAYER_DEADBIP,
             FO4_LAYER_BIPED_NO_CC,
         }) {
        for (const std::uint32_t damageLayer : {
                 FO4_LAYER_PROJECTILE,
                 FO4_LAYER_CONEPROJECTILE,
                 FO4_LAYER_SPELL,
                 FO4_LAYER_SPELLEXPLOSION,
             }) {
            Require(layerPairSymmetricMatches(
                        matrix.data(),
                        bipedLayer,
                        damageLayer,
                        true),
                "ROCK layer registration must preserve native BIPED damage pairs");
        }
    }

    for (const std::uint32_t damageLayer : {
             FO4_LAYER_PROJECTILE,
             FO4_LAYER_CONEPROJECTILE,
             FO4_LAYER_SPELL,
             FO4_LAYER_SPELLEXPLOSION,
         }) {
        Require(layerPairSymmetricMatches(
                    matrix.data(),
                    FO4_LAYER_CHARCONTROLLER,
                    damageLayer,
                    true),
            "the controller/object matrix policy must not disable native damage layers");
    }

    {
        auto disabledOptionalFilterMatrix = matrix;
        setPair(
            disabledOptionalFilterMatrix.data(),
            FO4_LAYER_CHARCONTROLLER,
            FO4_LAYER_BIPED_NO_CC,
            true);
        applyNativeCharacterControllerObjectSuppressionPolicy(
            disabledOptionalFilterMatrix.data(),
            false,
            ~std::uint64_t{ 0 },
            false);
        Require(layerPairSymmetricMatches(
                    disabledOptionalFilterMatrix.data(),
                    FO4_LAYER_CHARCONTROLLER,
                    FO4_LAYER_BIPED_NO_CC,
                    false),
            "turning off the optional contact filter must not break the held-body no-push invariant");
    }

    Require(!maskEnablesLayer(
                buildRockBodyExpectedMask(true),
                FO4_LAYER_PROJECTILE),
        "the generated ROCK body may reject projectiles only because native BIPED hitboxes remain authoritative");

    {
        using namespace rock::native_melee_suppression;

        const NativeMeleePolicyInput disabled{
            .rockEnabled = true,
            .suppressionEnabled = false,
            .fullSuppression = true,
            .suppressWeaponSwing = true,
            .suppressHitFrame = true,
            .actorIsPlayer = true,
        };
        Require(
            evaluateNativeMeleeSuppression(
                NativeMeleeEvent::WeaponSwing,
                disabled)
                    .action == NativeMeleeSuppressionAction::CallNative,
            "disabled suppression must pass native weapon swings through");
        Require(
            evaluateNativeMeleeSuppression(
                NativeMeleeEvent::HitFrame,
                disabled)
                    .action == NativeMeleeSuppressionAction::CallNative,
            "disabled suppression must pass native hit frames through");
        Require(
            evaluateNativeMeleeImpactSuppression(
                NativeMeleeImpactPolicyInput{
                    .rockEnabled = true,
                    .suppressionEnabled = false,
                    .fullSuppression = true,
                    .actorIsPlayer = true,
                })
                    .action == NativeMeleeImpactAction::CallNative,
            "disabled suppression must pass native VR melee impacts through");
        Require(
            evaluateNativeMeleeInputGate(
                NativeMeleeInputGatePolicyInput{
                    .rockEnabled = true,
                    .suppressionEnabled = false,
                    .fullSuppression = true,
                    .inputEvent = NativeMeleeInputEvent::RightStick,
                })
                    .action == NativeMeleeInputGateAction::CallNative,
            "disabled suppression must leave the native melee input gate open");
    }

    std::cout << "CollisionLayerPolicyTests passed\n";
    return 0;
}
