// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <AppInstallerArchitecture.h>

#include <string>
#include <string_view>


namespace AppInstaller::Utility
{
    // The specific version of normalization being used.
    enum class NormalizationVersion
    {
        Initial,
    };

    // The category of a word as determined by normalization.
    enum class NormalizedWordCategory
    {
        // A word that is descriptive of the name or publisher.
        Primary,
        // A word consisting of only whitespace.
        Whitespace,
        // A word that describes the architecture of the package.
        Architecture,
        // A word that describes the locale of the package.
        Locale,
        // A word that is not unique to the name or publisher.
        Ignorable,
    };

    // A package publisher and name that has been normalized, allowing direct
    // comparison across versions and many other facet. Also allows use in
    // generating and Id for local packages.
    struct NormalizedName
    {
        NormalizedName() = default;

        const std::string& Name() const { return m_name; }
        void Name(std::string&& name) { m_name = std::move(name); }
        void Name(std::string_view name) { m_name = name; }

        const std::vector<std::wstring>& NameWords() const { return m_nameWords; }
        void NameWords(std::vector<std::wstring>&& words) { m_nameWords = std::move(words); }

        Utility::Architecture Architecture() const { return m_arch; }
        void Architecture(Utility::Architecture arch) { m_arch = arch; }

        const std::string& Locale() const { return m_locale; }
        void Locale(std::string&& locale) { m_locale = std::move(locale); }

        const std::string& Publisher() const { return m_publisher; }
        void Publisher(std::string&& publisher) { m_publisher = std::move(publisher); }
        void Publisher(std::string_view publisher) { m_publisher = publisher; }

    private:
        std::string m_name;
        std::vector<std::wstring> m_nameWords;
        Utility::Architecture m_arch = Utility::Architecture::Unknown;
        std::string m_locale;
        std::string m_publisher;
    };

    namespace details
    {
        // NameNormalizer interface to allow different versions.
        struct INameNormalizer
        {
            virtual ~INameNormalizer() = default;

            virtual NormalizedName Normalize(std::string_view name, std::string_view publisher) const = 0;

            virtual NormalizedName NormalizeName(std::string_view name) const = 0;

            virtual std::string NormalizePublisher(std::string_view publisher) const = 0;

            virtual NormalizedWordCategory DetermineWordCategory(std::string_view name, bool isName) const = 0;
        };
    }

    // Helper that manages the lifetime of the internals required to
    // execute the name normalization.
    struct NameNormalizer
    {
        NameNormalizer(NormalizationVersion version);

        // Normalize both the name and publisher at the same time.
        NormalizedName Normalize(std::string_view name, std::string_view publisher) const;

        // Normalize only the name.
        NormalizedName NormalizeName(std::string_view name) const;

        // Normalize only the publisher.
        std::string NormalizePublisher(std::string_view publisher) const;

        // Determine the category of a given word.
        NormalizedWordCategory DetermineWordCategory(std::string_view value, bool isName) const;

    private:
        std::unique_ptr<details::INameNormalizer> m_normalizer;
    };
}
