// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "WorkflowBase.h"
#include "DependenciesFlow.h"
#include "InstallFlow.h"
#include "UpdateFlow.h"
#include "ManifestComparator.h"
#include <Microsoft/PinningIndex.h>

using namespace AppInstaller::Repository;
using namespace AppInstaller::Repository::Microsoft;
using namespace AppInstaller::Pinning;

namespace AppInstaller::CLI::Workflow
{
    namespace
    {
        bool IsUpdateVersionApplicable(const Utility::Version& installedVersion, const Utility::Version& updateVersion)
        {
            return installedVersion < updateVersion;
        }

        void AddToPackageSubContextsIfNotPresent(std::vector<std::unique_ptr<Execution::Context>>& packageSubContexts, std::unique_ptr<Execution::Context> packageContext)
        {
            for (auto const& existing : packageSubContexts)
            {
                if (existing->Get<Execution::Data::Manifest>().Id == packageContext->Get<Execution::Data::Manifest>().Id &&
                    existing->Get<Execution::Data::Manifest>().Version == packageContext->Get<Execution::Data::Manifest>().Version &&
                    existing->Get<Execution::Data::PackageVersion>()->GetProperty(PackageVersionProperty::SourceIdentifier) == packageContext->Get<Execution::Data::PackageVersion>()->GetProperty(PackageVersionProperty::SourceIdentifier))
                {
                    return;
                }
            }

            packageSubContexts.emplace_back(std::move(packageContext));
        }
    }

    void SelectLatestApplicableVersion::operator()(Execution::Context& context) const
    {
        auto package = context.Get<Execution::Data::Package>();
        auto installedPackage = context.Get<Execution::Data::InstalledPackageVersion>();
        const bool reportVersionNotFound = m_isSinglePackage;

        bool isUpgrade = WI_IsFlagSet(context.GetFlags(), Execution::ContextFlag::InstallerExecutionUseUpdate);;
        Utility::Version installedVersion;
        if (isUpgrade)
        {
            installedVersion = Utility::Version(installedPackage->GetProperty(PackageVersionProperty::Version));
        }

        ManifestComparator manifestComparator(context, isUpgrade ? installedPackage->GetMetadata() : IPackageVersion::Metadata{});
        bool versionFound = false;
        bool installedTypeInapplicable = false;
        bool packagePinned = false;

        if (isUpgrade && installedVersion.IsUnknown() && !context.Args.Contains(Execution::Args::Type::IncludeUnknown))
        {
            // the package has an unknown version and the user did not request to upgrade it anyway
            if (reportVersionNotFound)
            {
                context.Reporter.Info() << Resource::String::UpgradeUnknownVersionExplanation << std::endl;
            }

            AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE);
        }

        // If we are updating a single package or we got the --include-pinned flag,
        // we include packages with Pinning pins
        const bool includePinned = m_isSinglePackage || context.Args.Contains(Execution::Args::Type::IncludePinned);

        // The version keys should have already been sorted by version
        const auto& versionKeys = package->GetAvailableVersionKeys();
        for (const auto& key : versionKeys)
        {
            // Check Applicable Version
            if (!isUpgrade || IsUpdateVersionApplicable(installedVersion, Utility::Version(key.Version)))
            {
                // Check if the package is pinned
                if (key.PinnedState == Pinning::PinType::Blocking ||
                    key.PinnedState == Pinning::PinType::Gating ||
                    (key.PinnedState == Pinning::PinType::Pinning && !includePinned))
                {
                    AICLI_LOG(CLI, Info, << "Package [" << package->GetProperty(PackageProperty::Id) << " with Version[" << key.Version << "] from Source[" << key.SourceId << "] has a Pin with type[" << ToString(key.PinnedState) << "]");
                    if (context.Args.Contains(Execution::Args::Type::Force))
                    {
                        AICLI_LOG(CLI, Info, << "Ignoring pin due to --force argument");
                    }
                    else
                    {
                        packagePinned = true;
                        continue;
                    }
                }

                auto packageVersion = package->GetAvailableVersion(key);
                auto manifest = packageVersion->GetManifest();

                // Check applicable Installer
                auto [installer, inapplicabilities] = manifestComparator.GetPreferredInstaller(manifest);
                if (!installer.has_value())
                {
                    // If there is at least one installer whose only reason is InstalledType.
                    auto onlyInstalledType = std::find(inapplicabilities.begin(), inapplicabilities.end(), InapplicabilityFlags::InstalledType);
                    if (onlyInstalledType != inapplicabilities.end())
                    {
                        installedTypeInapplicable = true;
                    }

                    continue;
                }

                Logging::Telemetry().LogSelectedInstaller(
                    static_cast<int>(installer->Arch),
                    installer->Url,
                    Manifest::InstallerTypeToString(installer->EffectiveInstallerType()),
                    Manifest::ScopeToString(installer->Scope),
                    installer->Locale);

                Logging::Telemetry().LogManifestFields(
                    manifest.Id,
                    manifest.DefaultLocalization.Get<Manifest::Localization::PackageName>(),
                    manifest.Version);

                // Since we already did installer selection, just populate the context Data
                manifest.ApplyLocale(installer->Locale);
                context.Add<Execution::Data::Manifest>(std::move(manifest));
                context.Add<Execution::Data::PackageVersion>(std::move(packageVersion));
                context.Add<Execution::Data::Installer>(std::move(installer));

                versionFound = true;
                break;
            }
            else
            {
                // Any following versions are not applicable
                break;
            }
        }

        if (!versionFound)
        {
            if (reportVersionNotFound)
            {
                if (installedTypeInapplicable)
                {
                    context.Reporter.Info() << Resource::String::UpgradeDifferentInstallTechnologyInNewerVersions << std::endl;
                }
                else if (packagePinned)
                {
                    context.Reporter.Info() << Resource::String::UpgradeIsPinned << std::endl;
                    AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PACKAGE_IS_PINNED);
                }
                else if (isUpgrade)
                {
                    context.Reporter.Info() << Resource::String::UpdateNotApplicable << std::endl;
                }
                else
                {
                    context.Reporter.Error() << Resource::String::NoApplicableInstallers << std::endl;
                }
            }

            AICLI_TERMINATE_CONTEXT(isUpgrade ? APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE : APPINSTALLER_CLI_ERROR_NO_APPLICABLE_INSTALLER);
        }
    }

    void EnsureUpdateVersionApplicable(Execution::Context& context)
    {
        auto installedPackage = context.Get<Execution::Data::InstalledPackageVersion>();
        Utility::Version installedVersion = Utility::Version(installedPackage->GetProperty(PackageVersionProperty::Version));
        Utility::Version updateVersion(context.Get<Execution::Data::Manifest>().Version);

        if (!IsUpdateVersionApplicable(installedVersion, updateVersion))
        {
            context.Reporter.Info() << Resource::String::UpdateNotApplicable << std::endl;
            AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE);
        }
    }

    void UpdateAllApplicable(Execution::Context& context)
    {
        const auto& matches = context.Get<Execution::Data::SearchResult>().Matches;
        std::vector<std::unique_ptr<Execution::Context>> packageSubContexts;
        bool updateAllFoundUpdate = false;
        int packagesWithUnknownVersionSkipped = 0;
        int packagesThatRequireExplicitSkipped = 0;

        for (const auto& match : matches)
        {
            // We want to do best effort to update all applicable updates regardless on previous update failure
            auto updateContextPtr = context.CreateSubContext();
            Execution::Context& updateContext = *updateContextPtr;
            auto previousThreadGlobals = updateContext.SetForCurrentThread();
            auto installedVersion = match.Package->GetInstalledVersion();

            updateContext.Add<Execution::Data::Package>(match.Package);

            // Filter out packages with unknown installed versions
            if (context.Args.Contains(Execution::Args::Type::IncludeUnknown))
            {
                updateContext.Args.AddArg(Execution::Args::Type::IncludeUnknown);
            }
            else if (Utility::Version(installedVersion->GetProperty(PackageVersionProperty::Version)).IsUnknown())
            {
                // we don't know what the package's version is and the user didn't ask to upgrade it anyway.
                AICLI_LOG(CLI, Info, << "Skipping " << match.Package->GetProperty(PackageProperty::Id) << " as it has unknown installed version");
                ++packagesWithUnknownVersionSkipped;
                continue;
            }

            updateContext <<
                Workflow::GetInstalledPackageVersion <<
                Workflow::ReportExecutionStage(ExecutionStage::Discovery) <<
                SelectLatestApplicableVersion(false);

            if (updateContext.GetTerminationHR() == APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE)
            {
                continue;
            }

            // Filter out packages that require explicit upgrade.
            // User-defined pins are handled when selecting the version to use.
            auto installedMetadata = updateContext.Get<Execution::Data::InstalledPackageVersion>()->GetMetadata();
            auto pinnedState = ConvertToPinTypeEnum(installedMetadata[PackageVersionMetadata::PinnedState]);
            if (pinnedState == PinType::PinnedByManifest)
            {
                // Note that for packages pinned by the manifest
                // this does not consider whether the update to be installed has
                // RequireExplicitUpgrade. While this has the downside of not working with
                // packages installed from another source, it ensures consistency with the
                // list of available updates (there we don't have the selected installer)
                // and at most we will update each package like this once.
                AICLI_LOG(CLI, Info, << "Skipping " << match.Package->GetProperty(PackageProperty::Id) << " as it requires explicit upgrade");
                ++packagesThatRequireExplicitSkipped;
                continue;
            }

            updateAllFoundUpdate = true;

            AddToPackageSubContextsIfNotPresent(packageSubContexts, std::move(updateContextPtr));
        }

        if (updateAllFoundUpdate)
        {
            context.Add<Execution::Data::PackageSubContexts>(std::move(packageSubContexts));
            context.Reporter.Info() << std::endl;
            context <<
                InstallMultiplePackages(
                    Resource::String::InstallAndUpgradeCommandsReportDependencies,
                    APPINSTALLER_CLI_ERROR_UPDATE_ALL_HAS_FAILURE,
                    { APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE });
        }

        if (packagesWithUnknownVersionSkipped > 0)
        {
            AICLI_LOG(CLI, Info, << packagesWithUnknownVersionSkipped << " package(s) skipped due to unknown installed version");
            context.Reporter.Info() << Resource::String::UpgradeUnknownVersionCount(packagesWithUnknownVersionSkipped) << std::endl;
        }

        if (packagesThatRequireExplicitSkipped > 0)
        {
            AICLI_LOG(CLI, Info, << packagesThatRequireExplicitSkipped << " package(s) skipped due to requiring explicit upgrade");
            context.Reporter.Info() << Resource::String::UpgradeRequireExplicitCount(packagesThatRequireExplicitSkipped) << std::endl;
        }
    }

    void SelectSinglePackageVersionForInstallOrUpgrade::operator()(Execution::Context& context) const
    {
        context <<
            HandleSearchResultFailures <<
            EnsureOneMatchFromSearchResult(m_isUpgrade) <<
            GetInstalledPackageVersion;

        if (!m_isUpgrade && context.Contains(Execution::Data::InstalledPackageVersion) && context.Get<Execution::Data::InstalledPackageVersion>() != nullptr)
        {
            if (context.Args.Contains(Execution::Args::Type::NoUpgrade))
            {
                AICLI_LOG(CLI, Warning, << "Found installed package, exiting installation.");
                context.Reporter.Warn() << Resource::String::PackageAlreadyInstalled << std::endl;
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_PACKAGE_ALREADY_INSTALLED);
            }
            else
            {
                AICLI_LOG(CLI, Info, << "Found installed package, converting to upgrade flow");
                context.Reporter.Info() << Execution::ConvertToUpgradeFlowEmphasis << Resource::String::ConvertInstallFlowToUpgrade << std::endl;
                context.SetFlags(Execution::ContextFlag::InstallerExecutionUseUpdate);
                m_isUpgrade = true;
            }
        }

        if (context.Args.Contains(Execution::Args::Type::Version))
        {
            // If version specified, use the version and verify applicability
            context << GetManifestFromPackage(/* considerPins */ true);

            if (m_isUpgrade)
            {
                context << EnsureUpdateVersionApplicable;
            }

            context << SelectInstaller;
        }
        else
        {
            // Iterate through available versions to find latest applicable version.
            // This step also populates Manifest and Installer in context data.
            context << SelectLatestApplicableVersion(true);
        }

        context << EnsureApplicableInstaller;
    }

    void InstallOrUpgradeSinglePackage::operator()(Execution::Context& context) const
    {
        context <<
            SearchSourceForSingle <<
            SelectSinglePackageVersionForInstallOrUpgrade(m_isUpgrade) <<
            InstallSinglePackage;
    }
}
