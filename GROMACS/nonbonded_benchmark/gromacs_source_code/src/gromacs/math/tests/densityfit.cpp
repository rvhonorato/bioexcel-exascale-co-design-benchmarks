/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2019, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 * \brief
 * Tests density fitting routines.
 *
 * \author Christian Blau <blau@kth.se>
 * \ingroup module_math
 */
#include "gmxpre.h"

#include "gromacs/math/densityfit.h"

#include <numeric>

#include <gtest/gtest.h>

#include "gromacs/math/multidimarray.h"

#include "testutils/testasserts.h"
#include "testutils/testmatchers.h"

namespace gmx
{

namespace test
{

TEST(DensitySimilarityTest, InnerProductIsCorrect)
{
    MultiDimArray<std::vector<float>, dynamicExtents3D> referenceDensity(3, 3, 3);
    std::iota(begin(referenceDensity), end(referenceDensity), 0);

    DensitySimilarityMeasure measure(DensitySimilarityMeasureMethod::innerProduct, referenceDensity.asConstView());

    MultiDimArray<std::vector<float>, dynamicExtents3D> comparedDensity(3, 3, 3);
    std::iota(begin(comparedDensity), end(comparedDensity), -18);

    // 0*(-18) + 1*(-17) .. + 26 * 8 / Number elements
    const float expectedSimilarity = -117.0 / comparedDensity.asConstView().mapping().required_span_size();
    EXPECT_FLOAT_EQ(expectedSimilarity, measure.similarity(comparedDensity.asConstView()));
}

TEST(DensitySimilarityTest, InnerProductGradientIsCorrect)
{
    MultiDimArray<std::vector<float>, dynamicExtents3D> referenceDensity(3, 3, 3);
    std::iota(begin(referenceDensity), end(referenceDensity), 0);

    DensitySimilarityMeasure measure(DensitySimilarityMeasureMethod::innerProduct, referenceDensity.asConstView());

    MultiDimArray<std::vector<float>, dynamicExtents3D> comparedDensity(3, 3, 3);
    std::iota(begin(comparedDensity), end(comparedDensity), -18);

    std::vector<float> expectedSimilarityGradient;
    std::copy(begin(referenceDensity), end(referenceDensity),
              std::back_inserter(expectedSimilarityGradient));
    for (auto &x : expectedSimilarityGradient)
    {
        x /= comparedDensity.asConstView().mapping().required_span_size();
    }

    FloatingPointTolerance tolerance(defaultFloatTolerance());

    // Need this conversion to vector of float, because Pointwise requires size()
    // member function not provided by basic_mdspan
    const auto         gradient = measure.gradient(comparedDensity.asConstView());
    std::vector<float> gradientAsVector;
    gradientAsVector.assign(gradient.data(), gradient.data() + gradient.mapping().required_span_size());
    EXPECT_THAT(expectedSimilarityGradient, Pointwise(FloatEq(tolerance), gradientAsVector));
}

TEST(DensitySimilarityTest, GradientThrowsIfDensitiesDontMatch)
{
    MultiDimArray<std::vector<float>, dynamicExtents3D> referenceDensity(3, 3, 3);
    DensitySimilarityMeasure measure(DensitySimilarityMeasureMethod::innerProduct, referenceDensity.asConstView());

    MultiDimArray<std::vector<float>, dynamicExtents3D> comparedDensity(3, 3, 5);
    EXPECT_THROW(measure.gradient(comparedDensity.asConstView()), RangeError);
}

TEST(DensitySimilarityTest, SimilarityThrowsIfDensitiesDontMatch)
{
    MultiDimArray<std::vector<float>, dynamicExtents3D> referenceDensity(3, 3, 3);
    DensitySimilarityMeasure measure(DensitySimilarityMeasureMethod::innerProduct, referenceDensity.asConstView());
    MultiDimArray<std::vector<float>, dynamicExtents3D> comparedDensity(3, 3, 5);
    EXPECT_THROW(measure.similarity(comparedDensity.asConstView()), RangeError);
}

} // namespace test

} // namespace gmx
