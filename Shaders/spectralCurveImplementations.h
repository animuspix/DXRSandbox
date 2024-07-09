float polyMulBinomial(float x, float a, float b)
{
    // (ax [operator] b)^2
    // quick, lazy FOIL polynomial expansion
    float ax = a * x;
    float axSqr = ax * ax;
    float axb_2 = (ax * b) * 2.0f;
    float bSqr = b * b;

    // (ax - b)^2 should expand to
    // ax^2 - 2axb + b^2
    return axSqr - axb_2 + bSqr;
}

float quadratic(float x, float a, float b, float c, bool invert)
{
    float polynum = polyMulBinomial(x, a, b);
    return invert ? -polynum + c :
                     polynum + c;
}

float gaussian(float x, float a, float b, float c, float d)
{
    float polynum = polyMulBinomial(x, 1.0f, b);
    float frac = -(polynum / (2 * c * c));
    float gauss = exp(frac); // Unit gaussian
    return (gauss * a) - d; // Scale & translation
}