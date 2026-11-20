/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_SamplingOptions.cpp
 *
 * How an image is sampled when it is drawn at another size.
 *
 * What is worth pinning here is the *meaning* of each way of constructing one, because the fields overlap: an
 * anisotropic maximum, a cubic resampler and a filter mode are three ways of saying how to sample, and which one wins
 * is decided by the values rather than by the type. A caller that filled the fields in directly would have to know
 * that; a caller that uses the constructors does not.
 *
 * The comparison is the other thing: these are carried inside draw calls, so equality is what decides whether two
 * calls can be batched, and it has to mean "the same sampling" rather than "the same bytes".
 */

#include <graphics/SamplingOptions.h>

#include <gtest/gtest.h>

using namespace graphics;

// ==================================================================================================
// The defaults
// ==================================================================================================

TEST(SamplingOptionsTest, TheDefaultIsNearestWithNoMipmaps) {
  const SamplingOptions options;

  EXPECT_EQ(options.filter, FilterMode::kNearest);
  EXPECT_EQ(options.mipmap, MipmapMode::kNone);
  EXPECT_FALSE(options.useCubic);
  EXPECT_FALSE(options.isAniso());
  EXPECT_EQ(options.maxAniso, 0) << "zero is the value that means 'not anisotropic'";
}

// ==================================================================================================
// Building one
// ==================================================================================================

TEST(SamplingOptionsTest, AFilterModeAloneMeansNoMipmaps) {
  // The implicit conversion is deliberate: a caller that says "linear" has said everything it means, and making it
  // spell out a mipmap mode would put a word in every call that does not care.
  const SamplingOptions linear = FilterMode::kLinear;

  EXPECT_EQ(linear.filter, FilterMode::kLinear);
  EXPECT_EQ(linear.mipmap, MipmapMode::kNone);
  EXPECT_FALSE(linear.useCubic);
}

TEST(SamplingOptionsTest, BothModesTogether) {
  const SamplingOptions options(FilterMode::kLinear, MipmapMode::kLinear);

  EXPECT_EQ(options.filter, FilterMode::kLinear);
  EXPECT_EQ(options.mipmap, MipmapMode::kLinear);
}

TEST(SamplingOptionsTest, ACubicResamplerSelectsTheCubicFilter) {
  // Constructing from a resampler sets the flag that makes the sampler read the cubic parameters instead of the
  // filter and mipmap modes. A caller that forgot the flag would get nearest sampling and never know why its
  // downscale looked blocky.
  const SamplingOptions mitchell = CubicResampler::Mitchell();

  EXPECT_TRUE(mitchell.useCubic) << "a cubic resampler has to select cubic sampling";
  EXPECT_FLOAT_EQ(mitchell.cubic.b, 1.0f / 3.0f);
  EXPECT_FLOAT_EQ(mitchell.cubic.c, 1.0f / 3.0f);

  const SamplingOptions catmull = CubicResampler::CatmullRom();
  EXPECT_TRUE(catmull.useCubic);
  EXPECT_FLOAT_EQ(catmull.cubic.b, 0.0f);
  EXPECT_FLOAT_EQ(catmull.cubic.c, 0.5f);

  EXPECT_NE(mitchell, catmull) << "the two named filters are different filters";
}

TEST(SamplingOptionsTest, AnisotropyIsRaisedToAtLeastOne) {
  const SamplingOptions aniso = SamplingOptions::Aniso(8);
  EXPECT_TRUE(aniso.isAniso());
  EXPECT_EQ(aniso.maxAniso, 8);

  // Zero already means "no anisotropic sampling", so a maximum of zero is raised to one rather than stored. Storing
  // it would make `Aniso(0)` indistinguishable from the default while reading as a request that was honoured.
  const SamplingOptions zero = SamplingOptions::Aniso(0);
  EXPECT_EQ(zero.maxAniso, 1);
  EXPECT_TRUE(zero.isAniso());

  // A negative maximum would mean the same as zero while reading as a quantity, so it is raised too.
  EXPECT_EQ(SamplingOptions::Aniso(-4).maxAniso, 1);
}

// ==================================================================================================
// Comparing
// ==================================================================================================

TEST(SamplingOptionsTest, EqualityComparesTheSamplingNotTheBytes) {
  EXPECT_EQ(SamplingOptions(FilterMode::kLinear, MipmapMode::kNearest),
          SamplingOptions(FilterMode::kLinear, MipmapMode::kNearest));
  EXPECT_EQ(SamplingOptions {}, SamplingOptions {});

  EXPECT_NE(SamplingOptions(FilterMode::kLinear), SamplingOptions(FilterMode::kNearest));
  EXPECT_NE(SamplingOptions(FilterMode::kLinear, MipmapMode::kLinear),
          SamplingOptions(FilterMode::kLinear, MipmapMode::kNearest));

  // The cubic parameters take part: two cubic samplers that differ in B or C are different filters, and a batcher
  // that ignored them would draw one image twice as the other.
  CubicResampler slightlyDifferent {};
  slightlyDifferent.b = 0.1f;
  slightlyDifferent.c = 0.5f;
  EXPECT_NE(SamplingOptions(CubicResampler::CatmullRom()), SamplingOptions(slightlyDifferent));

  // And the cubic flag takes part, so a cubic sampler does not compare equal to a filt-mode one that happens to share
  // its numeric fields.
  SamplingOptions asCubic;
  asCubic.useCubic = true;
  asCubic.cubic = CubicResampler::Mitchell();
  asCubic.filter = FilterMode::kNearest;
  EXPECT_NE(asCubic, SamplingOptions(FilterMode::kNearest));
}

TEST(SamplingOptionsTest, CopyingAndAssigningPreserveTheSampling) {
  // Skia's version has `const` members and has to reconstruct itself in place to assign. Ours are ordinary members,
  // and this is the check that the generated operations do the right thing -- which the placement-new there would
  // have hidden.
  SamplingOptions original(FilterMode::kLinear, MipmapMode::kLinear);
  original.maxAniso = 4;

  SamplingOptions copy = original;
  EXPECT_EQ(copy, original);

  SamplingOptions assigned;
  assigned = original;
  EXPECT_EQ(assigned, original);
  EXPECT_EQ(assigned.maxAniso, 4);
  EXPECT_EQ(assigned.filter, FilterMode::kLinear);
  EXPECT_EQ(assigned.mipmap, MipmapMode::kLinear);

  // And the copy is a copy: changing it does not reach back into the original.
  assigned.filter = FilterMode::kNearest;
  EXPECT_EQ(original.filter, FilterMode::kLinear);
}
