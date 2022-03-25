// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "winget/ARPCorrelation.h"
#include "winget/Manifest.h"
#include "winget/NameNormalization.h"
#include "winget/RepositorySearch.h"

using namespace AppInstaller::Manifest;
using namespace AppInstaller::Repository;
using namespace AppInstaller::Utility;

namespace AppInstaller::Repository::Correlation
{
    namespace
    {
        struct EntryScore
        {
            ARPEntry Entry;
            double Score;

            EntryScore(
                const ARPCorrelationMeasure& measure,
                const Manifest::Manifest& manifest,
                const ARPEntry& entry)
                : Entry(entry), Score(measure.GetMatchingScore(manifest, entry)) {}

            bool operator<(const EntryScore& other)
            {
                return Score < other.Score;
            }
        };

        double EditDistanceScore(std::string_view sv1, std::string_view sv2)
        {
            // Naive implementation of Levenshtein distance (scaled over the string size)
            const double EditCost = 0.5;
            const double AddCost = 1;

            // Do it ignoring case
            auto s1 = Utility::ToLower(sv1);
            auto s2 = Utility::ToLower(sv2);

            // distance[i][j] = distance between s1[0:i] and s2[0:j]
            std::vector<std::vector<double>> distance{ s1.size(), std::vector<double>(s2.size(), 0.0) };

            for (size_t i = 0; i < s1.size(); ++i)
            {
                for (size_t j = 0; j < s2.size(); ++j)
                {
                    double& d = distance[i][j];
                    if (i == 0)
                    {
                        d = j * AddCost;
                    }
                    else if (j == 0)
                    {
                        d = i * AddCost;
                    }
                    else if (s1[i] == s2[j])
                    {
                        d = distance[i - 1][j - 1];
                    }
                    else
                    {
                        d = std::min(
                            EditCost + distance[i - 1][j - 1],
                            AddCost + std::min(distance[i][j - 1], distance[i - 1][j]));
                    }
                }
            }

            return 1 - distance.back().back() / std::max(s1.size(), s2.size());
        }
    }




    double ARPCorrelationMeasure::GetMatchingScore(
        const AppInstaller::Manifest::Manifest& manifest,
        const ARPEntry& arpEntry) const
    {
        double bestMatchingScore = GetMatchingScore(manifest, manifest.DefaultLocalization, arpEntry);
        for (const auto& localization : manifest.Localizations)
        {
            double matchingScore = GetMatchingScore(manifest, localization, arpEntry);
            bestMatchingScore = std::max(bestMatchingScore, matchingScore);
        }

        return bestMatchingScore;
    }

    std::optional<ARPEntry> ARPCorrelationMeasure::GetBestMatchForManifest(
        const Manifest::Manifest& manifest,
        const std::vector<ARPEntry>& arpEntries) const
    {
        AICLI_LOG(Repo, Verbose, << "Looking for best match in ARP for manifest " << manifest.Id);

        std::optional<ARPEntry> bestMatch;
        double bestScore = std::numeric_limits<double>::lowest();

        for (const auto& arpEntry : arpEntries)
        {
            auto score = GetMatchingScore(manifest, arpEntry);
            AICLI_LOG(Repo, Verbose, << "Match score for " << arpEntry.Entry->GetProperty(PackageVersionProperty::Id) << ": " << score);

            if (score < GetMatchingThreshold())
            {
                AICLI_LOG(Repo, Verbose, << "Score is lower than threshold");
                continue;
            }

            if (!bestMatch || bestScore < score)
            {
                bestMatch = arpEntry;
                bestScore = score;
            }
        }

        if (bestMatch)
        {
            AICLI_LOG(Repo, Verbose, << "Best match is " << bestMatch->Name);
        }
        else
        {
            AICLI_LOG(Repo, Verbose, << "No ARP entry had a correlation score surpassing the required threshold");
        }

        return bestMatch;
    }

    const ARPCorrelationMeasure& ARPCorrelationMeasure::GetInstance()
    {
        static NoCorrelation instance;
        return instance;
    }

    double NoCorrelation::GetMatchingScore(
        const Manifest::Manifest&,
        const ManifestLocalization&,
        const ARPEntry&) const
    {
        return 0;
    }

    double NoCorrelation::GetMatchingThreshold() const
    {
        return 1;
    }

    double NormalizedNameAndPublisherCorrelation::GetMatchingScore(
        const Manifest::Manifest&,
        const ManifestLocalization& localization,
        const ARPEntry& arpEntry) const
    {
        NameNormalizer normer(NormalizationVersion::Initial);

        auto arpNormalizedName = normer.Normalize(arpEntry.Name, arpEntry.Publisher);

        auto name = localization.Get<Localization::PackageName>();
        auto publisher = localization.Get<Localization::Publisher>();

        auto manifestNormalizedName = normer.Normalize(name, publisher);

        if (Utility::CaseInsensitiveEquals(arpNormalizedName.Name(), manifestNormalizedName.Name()) &&
            Utility::CaseInsensitiveEquals(arpNormalizedName.Publisher(), manifestNormalizedName.Publisher()))
        {
            return 1;
        }

        return 0;
    }

    double NormalizedNameAndPublisherCorrelation::GetMatchingThreshold() const
    {
        return 1;
    }

    double NormalizedEditDistanceCorrelation::GetMatchingScore(
        const Manifest::Manifest&,
        const ManifestLocalization& localization,
        const ARPEntry& arpEntry) const
    {
        NameNormalizer normer(NormalizationVersion::Initial);

        auto arpNormalizedName = normer.Normalize(arpEntry.Name, arpEntry.Publisher);

        auto name = localization.Get<Localization::PackageName>();
        auto publisher = localization.Get<Localization::Publisher>();

        auto manifestNormalizedName = normer.Normalize(name, publisher);

        auto nameDistance = EditDistanceScore(arpNormalizedName.Name(), manifestNormalizedName.Name());
        auto publisherDistance = EditDistanceScore(arpNormalizedName.Publisher(), manifestNormalizedName.Publisher());

        return nameDistance * publisherDistance;
    }

    double NormalizedEditDistanceCorrelation::GetMatchingThreshold() const
    {
        return 0.5;
    }
    
    // This correlation attempts to use a longest common substring algorithm on the words of the values being compared.
    // The thinking is:
    //  1. there is at a minimum one word that is strongly tied to the brand of the the product name and publisher
    //  2. that these branding words are unique
    //  3. that typos in those words will be corrected in the data, not by our matching.
    // 
    // The algorithm is applied to both name and publisher, but the results are treated separately:
    //  1. split the value into words
    //  2. fold the case of each word
    //  3. use the existing normalization regexes to find words that would be removed by normalization
    //      a. do not remove them, just mark them as such for future scoring (ignorable)
    //  4. use longest common substring (ne phrase) at the word level to find the longest phrases
    //  5. calculate a score based on how much of each value was covered by the longest phrase
    // 
    // The scoring will take into account the name and publisher separately. The goal thresholds are:
    //  1.00 : Exact match for all words
    //  0.90 : Complete match when ignorable words are ignored
    //  0.75 : Complete match on one of the values, score will decrease with every missed word in other value
    //  0.50 : Incomplete match for both values, score will decrease with every missed word
    //  0.00 : No matching words
    // 
    // The scores will be combined to target an overall threshold of 0.5 as a reasonable match and 0.75 as a confident match.
    // Initial investigation into creating functions to combine the values are:
    //  if (name + publisher < 1)
    //      score = constant * (name ^ power) * (1 - publisher)
    //  else
    //      score = (1 - (1 - name) ^ power) - constant * (1 - publisher) ^ power
    // Where the values are:
    //  constant    : the output score when the name is a perfect match (1.0) and the publisher is a perfect miss (0.0)
    //  power       : a value used to create non-linear progression of the score, increasing it effectively increases the
    //                output threshold by lowering further for already low scores
    // The goal of these functions is to create a case that when:
    //  publisher == 0 : A slow ramp to the constant value, because a perfect name match should be fairly confident even if the publisher doesn't
    //  publisher == 1 : A quick ramp as the name matches more, as perfect publisher match plus some name should be better than just some name
    //  name == 0      : Should always be 0
    //  name == 1      : A quick ramp from the constant value to 1 as the publisher matches more
    double WordCorrelation::GetMatchingScore(
        const Manifest::Manifest&,
        const ManifestLocalization& localization,
        const ARPEntry& arpEntry) const
    {
        NameNormalizer normer(NormalizationVersion::Initial);

        auto arpNormalizedName = normer.Normalize(arpEntry.Name, arpEntry.Publisher);

        auto name = localization.Get<Localization::PackageName>();
        auto publisher = localization.Get<Localization::Publisher>();

        auto manifestNormalizedName = normer.Normalize(name, publisher);

        auto nameDistance = EditDistanceScore(arpNormalizedName.Name(), manifestNormalizedName.Name());
        auto publisherDistance = EditDistanceScore(arpNormalizedName.Publisher(), manifestNormalizedName.Publisher());

        return nameDistance * publisherDistance;
    }

    double WordCorrelation::GetMatchingThreshold() const
    {
        return 0.5;
    }

}